/*	$NetBSD: iscsi_send.c,v 1.36 2017/12/03 19:07:10 christos Exp $	*/

/*-
 * Copyright (c) 2004,2005,2006,2011 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Wasabi Systems, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "iscsi_globals.h"

#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/atomic.h>

/*#define LUN_1  1 */

/*****************************************************************************/

/*
 * my_soo_write:
 *    Replacement for soo_write with flag handling.
 *
 *    Parameter:
 *          conn     The connection
 *          u        The uio descriptor
 *
 *    Returns:    0 on success, else EIO.
 */

STATIC int
my_soo_write(connection_t *conn, struct uio *u)
{
	struct socket *so = conn->c_sock->f_socket;
	int ret;
#ifdef ISCSI_DEBUG
	size_t resid = u->uio_resid;
#endif

	KASSERT(u->uio_resid != 0);

	ret = (*so->so_send)(so, NULL, u, NULL, NULL, 0, conn->c_threadobj);

	DEB(99, ("soo_write done: len = %zu\n", u->uio_resid));

	if (ret != 0 || u->uio_resid) {
		DEBC(conn, 0, ("Write failed sock %p (ret: %d, req: %zu, resid: %zu)\n",
			conn->c_sock, ret, resid, u->uio_resid));
		handle_connection_error(conn, ISCSI_STATUS_SOCKET_ERROR, NO_LOGOUT);
		return EIO;
	}
	return 0;
}

/*****************************************************************************/

/*
 * assign_connection:
 *    This function returns the connection to use for the next transaction.
 *
 *    Parameter:  The session
 *
 *    Returns:    The connection
 */

connection_t *
assign_connection(session_t *sess, bool waitok)
{
	connection_t *conn, *next;

	mutex_enter(&sess->s_lock);
	do {
		if (sess->s_terminating ||
		    (conn = sess->s_mru_connection) == NULL) {
			mutex_exit(&sess->s_lock);
			return NULL;
		}
		next = conn;
		do {
			next = TAILQ_NEXT(next, c_connections);
			if (next == NULL) {
				next = TAILQ_FIRST(&sess->s_conn_list);
			}
		} while (next != NULL && next != conn &&
				 next->c_state != ST_FULL_FEATURE);

		if (next->c_state != ST_FULL_FEATURE) {
			if (waitok) {
				cv_wait(&sess->s_sess_cv, &sess->s_lock);
				next = TAILQ_FIRST(&sess->s_conn_list);
			} else {
				mutex_exit(&sess->s_lock);
				return NULL;
			}
		} else {
			sess->s_mru_connection = next;
		}
	} while (next != NULL && next->c_state != ST_FULL_FEATURE);
	mutex_exit(&sess->s_lock);

	return next;
}


/*
 * reassign_tasks:
 *    Reassign pending commands to one of the still existing connections
 *    of a session.
 *
 *    Parameter:
 *          oldconn		The terminating connection
 */

STATIC void
reassign_tasks(connection_t *oldconn)
{
	session_t *sess = oldconn->c_session;
	connection_t *conn;
	ccb_t *ccb;
	pdu_t *pdu = NULL;
	pdu_t *opdu;
	int no_tm = 1;
	int rc = 1;
	uint32_t sn;

	if ((conn = assign_connection(sess, FALSE)) == NULL) {
		DEB(1, ("Reassign_tasks of Session %d, connection %d failed, "
			    "no active connection\n",
			    sess->s_id, oldconn->c_id));
		/* XXX here we need to abort the waiting CCBs */
		return;
	}

	if (sess->s_ErrorRecoveryLevel >= 2) {
		if (oldconn->c_loggedout == NOT_LOGGED_OUT) {
			oldconn->c_loggedout = LOGOUT_SENT;
			no_tm = send_logout(conn, oldconn, RECOVER_CONNECTION, TRUE);
			oldconn->c_loggedout = (rc) ? LOGOUT_FAILED : LOGOUT_SUCCESS;
			if (!oldconn->c_Time2Retain) {
				DEBC(conn, 1, ("Time2Retain is zero, setting no_tm\n"));
				no_tm = 1;
			}
		} else if (oldconn->c_loggedout == LOGOUT_SUCCESS) {
			no_tm = 0;
		}
		if (!no_tm && oldconn->c_Time2Wait) {
			DEBC(conn, 1, ("Time2Wait=%d, hz=%d, waiting...\n",
						   oldconn->c_Time2Wait, hz));
			kpause("Time2Wait", false, oldconn->c_Time2Wait * hz, NULL);
		}
	}

	DEBC(conn, 1, ("Reassign_tasks: Session %d, conn %d -> conn %d, no_tm=%d\n",
		sess->s_id, oldconn->c_id, conn->c_id, no_tm));


	/* XXX reassign waiting CCBs to new connection */

	while ((ccb = TAILQ_FIRST(&oldconn->c_ccbs_waiting)) != NULL) {
		/* Copy PDU contents (PDUs are bound to connection) */
		if ((pdu = get_pdu(conn, TRUE)) == NULL) {
			break;
		}

		/* adjust CCB and clone PDU for new connection */
		TAILQ_REMOVE(&oldconn->c_ccbs_waiting, ccb, ccb_chain);

		opdu = ccb->ccb_pdu_waiting;
		KASSERT((opdu->pdu_flags & PDUF_INQUEUE) == 0);

		*pdu = *opdu;

		/* restore overwritten back ptr */
		pdu->pdu_connection = conn;

		/* fixup saved UIO and IOVEC (regular one will be overwritten anyway) */
		pdu->pdu_save_uio.uio_iov = pdu->pdu_io_vec;
		pdu->pdu_save_iovec [0].iov_base = &pdu->pdu_hdr;

		if (conn->c_DataDigest && pdu->pdu_save_uio.uio_iovcnt > 1) {
			if (pdu->pdu_save_iovec [2].iov_base == NULL) {
				pdu->pdu_save_iovec [2].iov_base = &pdu->pdu_data_digest;
				pdu->pdu_save_uio.uio_iovcnt = 3;
			} else {
				pdu->pdu_save_iovec [3].iov_base = &pdu->pdu_data_digest;
				pdu->pdu_save_uio.uio_iovcnt = 4;
			}
		}
		pdu->pdu_save_iovec [0].iov_len =
			(conn->c_HeaderDigest) ? BHS_SIZE + 4 : BHS_SIZE;

		/* link new PDU into old CCB */
		ccb->ccb_pdu_waiting = pdu;
		/* link new CCB into new connection */
		ccb->ccb_connection = conn;
		/* reset timeouts */
		ccb->ccb_num_timeouts = 0;

		/* fixup reference counts */
		oldconn->c_usecount--;
		atomic_inc_uint(&conn->c_usecount);

		DEBC(conn, 1, ("CCB %p: Copied PDU %p to %p\n",
					   ccb, opdu, pdu));

		/* kill temp pointer that is now referenced by the new PDU */
		opdu->pdu_temp_data = NULL;

		/* and free the old PDU */
		free_pdu(opdu);

		/* put ready CCB into waiting list of new connection */
		mutex_enter(&conn->c_lock);
		suspend_ccb(ccb, TRUE);
		mutex_exit(&conn->c_lock);
	}

	if (pdu == NULL) {
		DEBC(conn, 1, ("Error while copying PDUs in reassign_tasks!\n"));
		/* give up recovering, the other connection is screwed up as well... */
		while ((ccb = TAILQ_FIRST(&oldconn->c_ccbs_waiting)) != NULL) {
			wake_ccb(ccb, oldconn->c_terminating);
		}
		/* XXX some CCBs might have been moved to new connection, but how is the
		 * new connection handled or killed ? */
		return;
	}

	TAILQ_FOREACH(ccb, &conn->c_ccbs_waiting, ccb_chain) {
		if (!no_tm) {
			rc = send_task_management(conn, ccb, NULL, TASK_REASSIGN);
		}
		/* if we get an error on reassign, restart the original request */
		if (no_tm || rc) {
			mutex_enter(&sess->s_lock);
			if (ccb->ccb_CmdSN < sess->s_ExpCmdSN) {
				pdu = ccb->ccb_pdu_waiting;
				sn = get_sernum(sess, pdu);

				/* update CmdSN */
				DEBC(conn, 1, ("Resend Updating CmdSN - old %d, new %d\n",
					   ccb->ccb_CmdSN, sn));
				ccb->ccb_CmdSN = sn;
				pdu->pdu_hdr.pduh_p.command.CmdSN = htonl(ccb->ccb_CmdSN);
			}
			mutex_exit(&sess->s_lock);
			resend_pdu(ccb);
		} else {
			ccb_timeout_start(ccb, COMMAND_TIMEOUT);
		}
		DEBC(conn, 1, ("Reassign ccb %p, no_tm=%d, rc=%d\n",
					   ccb, no_tm, rc));
	}
}


/*
 * iscsi_send_thread:
 *    This thread services the send queue, writing the PDUs to the socket.
 *    It also handles the cleanup when the connection is terminated.
 *
 *    Parameter:
 *          par		The connection this thread services
 */

void
iscsi_send_thread(void *par)
{
	connection_t *conn = (connection_t *) par;
	session_t *sess;
	ccb_t *ccb, *nccb;
	pdu_t *pdu;
	struct file *fp;
	pdu_disp_t pdisp;

	sess = conn->c_session;
	/* so cleanup thread knows there's someone left */
	iscsi_num_send_threads++;

	do {
		mutex_enter(&conn->c_lock);
		while (!conn->c_terminating) {
			while (!conn->c_terminating &&
				(pdu = TAILQ_FIRST(&conn->c_pdus_to_send)) != NULL) {
				TAILQ_REMOVE(&conn->c_pdus_to_send, pdu, pdu_send_chain);
				pdu->pdu_flags &= ~PDUF_INQUEUE;
				mutex_exit(&conn->c_lock);

				/* update ExpStatSN here to avoid discontinuities */
				/* and delays in updating target */
				pdu->pdu_hdr.pduh_p.command.ExpStatSN = htonl(conn->c_StatSN_buf.ExpSN);

				if (conn->c_HeaderDigest)
					pdu->pdu_hdr.pduh_HeaderDigest = gen_digest(&pdu->pdu_hdr, BHS_SIZE);

				DEBC(conn, 99, ("Transmitting PDU CmdSN = %u, ExpStatSN = %u\n",
				                ntohl(pdu->pdu_hdr.pduh_p.command.CmdSN),
				                ntohl(pdu->pdu_hdr.pduh_p.command.ExpStatSN)));
				my_soo_write(conn, &pdu->pdu_uio);

				mutex_enter(&conn->c_lock);
				pdisp = pdu->pdu_disp;
				if (pdisp > PDUDISP_FREE)
					pdu->pdu_flags &= ~PDUF_BUSY;
				mutex_exit(&conn->c_lock);
				if (pdisp <= PDUDISP_FREE)
					free_pdu(pdu);

				mutex_enter(&conn->c_lock);
			}

			if (!conn->c_terminating)
				cv_wait(&conn->c_conn_cv, &conn->c_lock);
		}
		mutex_exit(&conn->c_lock);

		/* ------------------------------------------------------------------------
		 *    Here this thread takes over cleanup of the terminating connection.
		 * ------------------------------------------------------------------------
		 */
		connection_timeout_stop(conn);
		conn->c_idle_timeout_val = CONNECTION_IDLE_TIMEOUT;

		fp = conn->c_sock;

		/*
		 * We shutdown the socket here to force the receive
		 * thread to wake up
		 */
		DEBC(conn, 1, ("Closing Socket %p\n", conn->c_sock));
		solock(fp->f_socket);
		soshutdown(fp->f_socket, SHUT_RDWR);
		sounlock(fp->f_socket);

		/* wake up any non-reassignable waiting CCBs */
		TAILQ_FOREACH_SAFE(ccb, &conn->c_ccbs_waiting, ccb_chain, nccb) {
			if (!(ccb->ccb_flags & CCBF_REASSIGN) || ccb->ccb_pdu_waiting == NULL) {
				DEBC(conn, 1, ("Terminating CCB %p (t=%p)\n",
					ccb,&ccb->ccb_timeout));
				wake_ccb(ccb, conn->c_terminating);
			} else {
				ccb_timeout_stop(ccb);
				ccb->ccb_num_timeouts = 0;
			}
		}

		/* clean out anything left in send queue */
		mutex_enter(&conn->c_lock);
		while ((pdu = TAILQ_FIRST(&conn->c_pdus_to_send)) != NULL) {
			TAILQ_REMOVE(&conn->c_pdus_to_send, pdu, pdu_send_chain);
			pdu->pdu_flags &= ~(PDUF_INQUEUE | PDUF_BUSY);
			mutex_exit(&conn->c_lock);
			/* if it's not attached to a waiting CCB, free it */
			if (pdu->pdu_owner == NULL ||
			    pdu->pdu_owner->ccb_pdu_waiting != pdu) {
				free_pdu(pdu);
			}
			mutex_enter(&conn->c_lock);
		}
		mutex_exit(&conn->c_lock);

		/* If there's another connection available, transfer pending tasks */
		if (sess->s_active_connections &&
			TAILQ_FIRST(&conn->c_ccbs_waiting) != NULL) {
			
			reassign_tasks(conn);
		} else if (!conn->c_destroy && conn->c_Time2Wait) {
			DEBC(conn, 1, ("Time2Wait\n"));
			kpause("Time2Wait", false, conn->c_Time2Wait * hz, NULL);
			DEBC(conn, 1, ("Time2Wait\n"));
		}
		/* notify event handlers of connection shutdown */
		DEBC(conn, 1, ("%s\n", conn->c_destroy ? "TERMINATED" : "RECOVER"));
		add_event(conn->c_destroy ? ISCSI_CONNECTION_TERMINATED
					  : ISCSI_RECOVER_CONNECTION,
				  sess->s_id, conn->c_id, conn->c_terminating);

		DEBC(conn, 1, ("Waiting for conn_idle\n"));
		mutex_enter(&conn->c_lock);
		if (!conn->c_destroy)
			cv_timedwait(&conn->c_idle_cv, &conn->c_lock, CONNECTION_IDLE_TIMEOUT);
		mutex_exit(&conn->c_lock);
		DEBC(conn, 1, ("Waited for conn_idle, destroy = %d\n", conn->c_destroy));

	} while (!conn->c_destroy);

	/* wake up anyone waiting for a PDU */
	mutex_enter(&conn->c_lock);
	cv_broadcast(&conn->c_conn_cv);
	mutex_exit(&conn->c_lock);

	/* wake up any waiting CCBs */
	while ((ccb = TAILQ_FIRST(&conn->c_ccbs_waiting)) != NULL) {
		KASSERT(ccb->ccb_disp >= CCBDISP_NOWAIT);
		wake_ccb(ccb, conn->c_terminating);
		/* NOTE: wake_ccb will remove the CCB from the queue */
	}

	add_connection_cleanup(conn);

	conn->c_sendproc = NULL;
	DEBC(conn, 1, ("Send thread exits\n"));
	iscsi_num_send_threads--;
	kthread_exit(0);
}


/*
 * send_pdu:
 *    Enqueue a PDU to be sent, and handle its disposition as well as
 *    the disposition of its associated CCB.
 *
 *    Parameter:
 *          ccb      The associated CCB. May be NULL if cdisp is CCBDISP_NOWAIT
 *                   and pdisp is not PDUDISP_WAIT
 *          cdisp    The CCB's disposition
 *          pdu      The PDU
 *          pdisp    The PDU's disposition
 */

STATIC void
send_pdu(ccb_t *ccb, pdu_t *pdu, ccb_disp_t cdisp, pdu_disp_t pdisp)
{
	connection_t *conn = pdu->pdu_connection;
	ccb_disp_t prev_cdisp = 0;

	if (ccb != NULL) {
		prev_cdisp = ccb->ccb_disp;
		pdu->pdu_hdr.pduh_InitiatorTaskTag = ccb->ccb_ITT;
		pdu->pdu_owner = ccb;
		if (cdisp != CCBDISP_NOWAIT)
			ccb->ccb_disp = cdisp;
	}

	pdu->pdu_disp = pdisp;

	DEBC(conn, 10, ("Send_pdu: CmdSN=%u ExpStatSN~%u ccb=%p, pdu=%p\n",
	                ntohl(pdu->pdu_hdr.pduh_p.command.CmdSN),
			conn->c_StatSN_buf.ExpSN,
			ccb, pdu));

	mutex_enter(&conn->c_lock);
	if (pdisp == PDUDISP_WAIT) {
		KASSERT(ccb != NULL);

		ccb->ccb_pdu_waiting = pdu;

		/* save UIO and IOVEC for retransmit */
		pdu->pdu_save_uio = pdu->pdu_uio;
		memcpy(pdu->pdu_save_iovec, pdu->pdu_io_vec, sizeof(pdu->pdu_save_iovec));

		pdu->pdu_flags |= PDUF_BUSY;
	}
	/* Enqueue for sending */
	pdu->pdu_flags |= PDUF_INQUEUE;

	if (pdu->pdu_flags & PDUF_PRIORITY)
		TAILQ_INSERT_HEAD(&conn->c_pdus_to_send, pdu, pdu_send_chain);
	else
		TAILQ_INSERT_TAIL(&conn->c_pdus_to_send, pdu, pdu_send_chain);

	cv_broadcast(&conn->c_conn_cv);

	if (cdisp != CCBDISP_NOWAIT) {
		KASSERT(ccb != NULL);
		KASSERTMSG(ccb->ccb_connection == conn, "conn mismatch %p != %p\n", ccb->ccb_connection, conn);

		if (prev_cdisp <= CCBDISP_NOWAIT)
			suspend_ccb(ccb, TRUE);

		mutex_exit(&conn->c_lock);
		ccb_timeout_start(ccb, COMMAND_TIMEOUT);
		mutex_enter(&conn->c_lock);

		while (ccb->ccb_disp == CCBDISP_WAIT) {
			DEBC(conn, 15, ("Send_pdu: ccb=%p cdisp=%d waiting\n",
				ccb, ccb->ccb_disp));
			cv_wait(&conn->c_ccb_cv, &conn->c_lock);
			DEBC(conn, 15, ("Send_pdu: ccb=%p cdisp=%d returned\n",
				ccb, ccb->ccb_disp));
		}
	}

	mutex_exit(&conn->c_lock);
}


/*
 * resend_pdu:
 *    Re-Enqueue a PDU that has apparently gotten lost.
 *
 *    Parameter:
 *          ccb      The associated CCB.
 */

void
resend_pdu(ccb_t *ccb)
{
	connection_t *conn = ccb->ccb_connection;
	pdu_t *pdu = ccb->ccb_pdu_waiting;

	mutex_enter(&conn->c_lock);
	if (pdu == NULL || (pdu->pdu_flags & PDUF_BUSY)) {
		mutex_exit(&conn->c_lock);
		return;
	}
	pdu->pdu_flags |= PDUF_BUSY;
	mutex_exit(&conn->c_lock);

	/* restore UIO and IOVEC */
	pdu->pdu_uio = pdu->pdu_save_uio;
	memcpy(pdu->pdu_io_vec, pdu->pdu_save_iovec, sizeof(pdu->pdu_io_vec));

	DEBC(conn, 8, ("ReSend_pdu: CmdSN=%u ExpStatSN~%u ccb=%p, pdu=%p\n",
	                ntohl(pdu->pdu_hdr.pduh_p.command.CmdSN),
			conn->c_StatSN_buf.ExpSN,
			ccb, pdu));

	mutex_enter(&conn->c_lock);
	/* Enqueue for sending */
	pdu->pdu_flags |= PDUF_INQUEUE;

	if (pdu->pdu_flags & PDUF_PRIORITY) {
		TAILQ_INSERT_HEAD(&conn->c_pdus_to_send, pdu, pdu_send_chain);
	} else {
		TAILQ_INSERT_TAIL(&conn->c_pdus_to_send, pdu, pdu_send_chain);
	}
	ccb_timeout_start(ccb, COMMAND_TIMEOUT);
	cv_broadcast(&conn->c_conn_cv);
	mutex_exit(&conn->c_lock);
}


/*
 * setup_tx_uio:
 *    Initialize the uio structure for sending, including header,
 *    data (if present), padding, and Data Digest.
 *    Header Digest is generated in send thread.
 *
 *    Parameter:
 *          pdu      The PDU
 *          dsl      The Data Segment Length
 *          data     The data pointer
 *          read     TRUE if this is a read operation
 */

STATIC void
setup_tx_uio(pdu_t *pdu, uint32_t dsl, void *data, bool read)
{
	static uint8_t pad_bytes[4] = { 0 };
	struct uio *uio;
	int i, pad, hlen;
	connection_t *conn = pdu->pdu_connection;

	DEB(99, ("SetupTxUio: dlen = %d, dptr: %p, read: %d\n",
			 dsl, data, read));

	if (!read && dsl) {
		hton3(dsl, pdu->pdu_hdr.pduh_DataSegmentLength);
	}
	hlen = (conn->c_HeaderDigest) ? BHS_SIZE + 4 : BHS_SIZE;

	pdu->pdu_io_vec[0].iov_base = &pdu->pdu_hdr;
	pdu->pdu_io_vec[0].iov_len = hlen;

	uio = &pdu->pdu_uio;

	uio->uio_iov = pdu->pdu_io_vec;
	uio->uio_iovcnt = 1;
	uio->uio_rw = UIO_WRITE;
	uio->uio_resid = hlen;
	UIO_SETUP_SYSSPACE(uio);

	if (!read && dsl) {
		uio->uio_iovcnt++;
		pdu->pdu_io_vec[1].iov_base = data;
		pdu->pdu_io_vec[1].iov_len = dsl;
		uio->uio_resid += dsl;

		/* Pad to next multiple of 4 */
		pad = uio->uio_resid & 0x03;
		if (pad) {
			i = uio->uio_iovcnt++;
			pad = 4 - pad;
			pdu->pdu_io_vec[i].iov_base = pad_bytes;
			pdu->pdu_io_vec[i].iov_len = pad;
			uio->uio_resid += pad;
		}

		if (conn->c_DataDigest) {
			pdu->pdu_data_digest = gen_digest_2(data, dsl, pad_bytes, pad);
			i = uio->uio_iovcnt++;
			pdu->pdu_io_vec[i].iov_base = &pdu->pdu_data_digest;
			pdu->pdu_io_vec[i].iov_len = 4;
			uio->uio_resid += 4;
		}
	}
}

/*
 * init_login_pdu:
 *    Initialize the login PDU.
 *
 *    Parameter:
 *          conn     The connection
 *          ccb      The CCB
 *          pdu      The PDU
 */

STATIC void
init_login_pdu(connection_t *conn, ccb_t *ccb, pdu_t *ppdu, bool next)
{
	pdu_header_t *hpdu = &ppdu->pdu_hdr;
	login_isid_t *isid = (login_isid_t *) & hpdu->pduh_LUN;
	uint8_t c_phase;

	hpdu->pduh_Opcode = IOP_Login_Request | OP_IMMEDIATE;

	mutex_enter(&conn->c_session->s_lock);
	ccb->ccb_CmdSN = get_sernum(conn->c_session, ppdu);
	mutex_exit(&conn->c_session->s_lock);

	if (next) {
		c_phase = (hpdu->pduh_Flags >> CSG_SHIFT) & SG_MASK;
		hpdu->pduh_Flags = FLAG_TRANSIT | (c_phase << CSG_SHIFT) |
					 NEXT_PHASE(c_phase);
	}

	memcpy(isid, &iscsi_InitiatorISID, 6);
	isid->TSIH = conn->c_session->s_TSIH;

	hpdu->pduh_p.login_req.CID = htons(conn->c_id);
	hpdu->pduh_p.login_req.CmdSN = htonl(ccb->ccb_CmdSN);
}


/*
 * negotiate_login:
 *    Control login negotiation.
 *
 *    Parameter:
 *          conn     The connection
 *          rx_pdu   The received login response PDU
 *          tx_ccb   The originally sent login CCB
 */

void
negotiate_login(connection_t *conn, pdu_t *rx_pdu, ccb_t *tx_ccb)
{
	int rc;
	bool next = TRUE;
	pdu_t *tx_pdu;
	uint8_t c_phase;

	if (rx_pdu->pdu_hdr.pduh_Flags & FLAG_TRANSIT)
		c_phase = rx_pdu->pdu_hdr.pduh_Flags & SG_MASK;
	else
		c_phase = (rx_pdu->pdu_hdr.pduh_Flags >> CSG_SHIFT) & SG_MASK;

	DEB(99, ("NegotiateLogin: Flags=%x Phase=%x\n",
			 rx_pdu->pdu_hdr.pduh_Flags, c_phase));

	if (c_phase == SG_FULL_FEATURE_PHASE) {
		session_t *sess = conn->c_session;

		if (!sess->s_TSIH)
			sess->s_TSIH = ((login_isid_t *) &rx_pdu->pdu_hdr.pduh_LUN)->TSIH;

		if (rx_pdu->pdu_temp_data != NULL)
			assemble_negotiation_parameters(conn, tx_ccb, rx_pdu, NULL);

		/* negotiated values are now valid */
		set_negotiated_parameters(tx_ccb);

		DEBC(conn, 5, ("Login Successful!\n"));
		wake_ccb(tx_ccb, ISCSI_STATUS_SUCCESS);
		return;
	}

	tx_pdu = get_pdu(conn, TRUE);
	if (tx_pdu == NULL)
		return;

	tx_pdu->pdu_hdr.pduh_Flags = c_phase << CSG_SHIFT;

	switch (c_phase) {
	case SG_SECURITY_NEGOTIATION:
		rc = assemble_security_parameters(conn, tx_ccb, rx_pdu, tx_pdu);
		if (rc < 0)
			next = FALSE;
		break;

	case SG_LOGIN_OPERATIONAL_NEGOTIATION:
		rc = assemble_negotiation_parameters(conn, tx_ccb, rx_pdu, tx_pdu);
		break;

	default:
		DEBOUT(("Invalid phase %x in negotiate_login\n", c_phase));
		rc = ISCSI_STATUS_TARGET_ERROR;
		break;
	}

	if (rc > 0) {
		wake_ccb(tx_ccb, rc);
		free_pdu(tx_pdu);
	} else {
		init_login_pdu(conn, tx_ccb, tx_pdu, next);
		setup_tx_uio(tx_pdu, tx_pdu->pdu_temp_data_len, tx_pdu->pdu_temp_data, FALSE);
		send_pdu(tx_ccb, tx_pdu, CCBDISP_NOWAIT, PDUDISP_FREE);
	}
}


/*
 * init_text_pdu:
 *    Initialize the text PDU.
 *
 *    Parameter:
 *          conn     The connection
 *          ccb      The transmit CCB
 *          ppdu     The transmit PDU
 *          rx_pdu   The received PDU if this is an unsolicited negotiation
 */

STATIC void
init_text_pdu(connection_t *conn, ccb_t *ccb, pdu_t *ppdu, pdu_t *rx_pdu)
{
	pdu_header_t *hpdu = &ppdu->pdu_hdr;

	hpdu->pduh_Opcode = IOP_Text_Request | OP_IMMEDIATE;
	hpdu->pduh_Flags = FLAG_FINAL;

	mutex_enter(&conn->c_session->s_lock);
	ccb->ccb_CmdSN = get_sernum(conn->c_session, ppdu);
	mutex_exit(&conn->c_session->s_lock);

	if (rx_pdu != NULL) {
		hpdu->pduh_p.text_req.TargetTransferTag =
			rx_pdu->pdu_hdr.pduh_p.text_rsp.TargetTransferTag;
		hpdu->pduh_LUN = rx_pdu->pdu_hdr.pduh_LUN;
	} else
		hpdu->pduh_p.text_req.TargetTransferTag = 0xffffffff;

	hpdu->pduh_p.text_req.CmdSN = htonl(ccb->ccb_CmdSN);
}


/*
 * acknowledge_text:
 *    Acknowledge a continued login or text response.
 *
 *    Parameter:
 *          conn     The connection
 *          rx_pdu   The received login/text response PDU
 *          tx_ccb   The originally sent login/text request CCB
 */

void
acknowledge_text(connection_t *conn, pdu_t *rx_pdu, ccb_t *tx_ccb)
{
	pdu_t *tx_pdu;

	tx_pdu = get_pdu(conn, TRUE);
	if (tx_pdu == NULL)
		return;

	if (rx_pdu != NULL &&
		(rx_pdu->pdu_hdr.pduh_Opcode & OPCODE_MASK) == IOP_Login_Request)
		init_login_pdu(conn, tx_ccb, tx_pdu, FALSE);
	else
		init_text_pdu(conn, tx_ccb, tx_pdu, rx_pdu);

	setup_tx_uio(tx_pdu, 0, NULL, FALSE);
	send_pdu(tx_ccb, tx_pdu, CCBDISP_NOWAIT, PDUDISP_FREE);
}


/*
 * start_text_negotiation:
 *    Handle target request to negotiate (via asynch event)
 *
 *    Parameter:
 *          conn     The connection
 */

void
start_text_negotiation(connection_t *conn)
{
	pdu_t *pdu;
	ccb_t *ccb;

	ccb = get_ccb(conn, TRUE);
	if (ccb == NULL)
		return;
	pdu = get_pdu(conn, TRUE);
	if (pdu == NULL) {
		free_ccb(ccb);
		return;
	}

	if (init_text_parameters(conn, ccb)) {
		free_ccb(ccb);
		free_pdu(pdu);
		return;
	}

	init_text_pdu(conn, ccb, pdu, NULL);
	setup_tx_uio(pdu, 0, NULL, FALSE);
	send_pdu(ccb, pdu, CCBDISP_FREE, PDUDISP_WAIT);
}


/*
 * negotiate_text:
 *    Handle received text negotiation.
 *
 *    Parameter:
 *          conn     The connection
 *          rx_pdu   The received text response PDU
 *          tx_ccb   The original CCB
 */

void
negotiate_text(connection_t *conn, pdu_t *rx_pdu, ccb_t *tx_ccb)
{
	int rc;
	pdu_t *tx_pdu;

	if (tx_ccb->ccb_flags & CCBF_SENDTARGET) {
		if (!(rx_pdu->pdu_hdr.pduh_Flags & FLAG_FINAL)) {
			handle_connection_error(conn, ISCSI_STATUS_PROTOCOL_ERROR,
									LOGOUT_CONNECTION);
			return;
		}
		/* transfer ownership of text to CCB */
		tx_ccb->ccb_text_data = rx_pdu->pdu_temp_data;
		tx_ccb->ccb_text_len = rx_pdu->pdu_temp_data_len;
		rx_pdu->pdu_temp_data = NULL;
		wake_ccb(tx_ccb, ISCSI_STATUS_SUCCESS);
	} else {
		if (!(rx_pdu->pdu_hdr.pduh_Flags & FLAG_FINAL))
			tx_pdu = get_pdu(conn, TRUE);
		else
			tx_pdu = NULL;

		rc = assemble_negotiation_parameters(conn, tx_ccb, rx_pdu, tx_pdu);
		if (rc) {
			if (tx_pdu != NULL)
				free_pdu(tx_pdu);

			handle_connection_error(conn, rc, LOGOUT_CONNECTION);
		} else if (tx_pdu != NULL) {
			init_text_pdu(conn, tx_ccb, tx_pdu, rx_pdu);
			setup_tx_uio(tx_pdu, tx_pdu->pdu_temp_data_len,
			     tx_pdu->pdu_temp_data, FALSE);
			send_pdu(tx_ccb, tx_pdu, CCBDISP_NOWAIT, PDUDISP_FREE);
		} else {
			set_negotiated_parameters(tx_ccb);
			wake_ccb(tx_ccb, ISCSI_STATUS_SUCCESS);
		}
	}
}


/*
 * send_send_targets:
 *    Send out a SendTargets text request.
 *    The result is stored in the fields in the session structure.
 *
 *    Parameter:
 *          session  The session
 *          key      The text key to use
 *
 *    Returns:    0 on success, else an error code.
 */

int
send_send_targets(session_t *sess, uint8_t *key)
{
	ccb_t *ccb;
	pdu_t *pdu;
	int rc = 0;
	connection_t *conn;

	DEB(9, ("Send_send_targets\n"));

	conn = assign_connection(sess, TRUE);
	if (conn == NULL || conn->c_terminating || conn->c_state != ST_FULL_FEATURE)
		return (conn != NULL && conn->c_terminating) ? conn->c_terminating
			: ISCSI_STATUS_CONNECTION_FAILED;

	ccb = get_ccb(conn, TRUE);
	if (ccb == NULL)
		return conn->c_terminating;
	pdu = get_pdu(conn, TRUE);
	if (pdu == NULL) {
		free_ccb(ccb);
		return conn->c_terminating;
	}

	ccb->ccb_flags |= CCBF_SENDTARGET;

	if ((rc = assemble_send_targets(pdu, key)) != 0) {
		free_ccb(ccb);
		free_pdu(pdu);
		return rc;
	}

	init_text_pdu(conn, ccb, pdu, NULL);

	setup_tx_uio(pdu, pdu->pdu_temp_data_len, pdu->pdu_temp_data, FALSE);
	send_pdu(ccb, pdu, CCBDISP_WAIT, PDUDISP_WAIT);

	rc = ccb->ccb_status;
	if (!rc) {
		/* transfer ownership of data */
		sess->s_target_list = ccb->ccb_text_data;
		sess->s_target_list_len = ccb->ccb_text_len;
		ccb->ccb_text_data = NULL;
	}
	free_ccb(ccb);
	return rc;
}


/*
 * send_nop_out:
 *    Send nop out request.
 *
 *    Parameter:
 *          conn     The connection
 *          rx_pdu   The received Nop-In PDU
 *
 *    Returns:    0 on success, else an error code.
 */

int
send_nop_out(connection_t *conn, pdu_t *rx_pdu)
{
	session_t *sess;
	ccb_t *ccb;
	pdu_t *ppdu;
	pdu_header_t *hpdu;
	uint32_t sn;

	if (rx_pdu != NULL) {
		ccb = NULL;
		ppdu = get_pdu(conn, TRUE);
		if (ppdu == NULL)
			return 1;
	} else {
		ccb = get_ccb(conn, FALSE);
		if (ccb == NULL) {
			DEBOUT(("Can't get CCB in send_nop_out\n"));
			return 1;
		}
		ppdu = get_pdu(conn, FALSE);
		if (ppdu == NULL) {
			free_ccb(ccb);
			DEBOUT(("Can't get PDU in send_nop_out\n"));
			return 1;
		}
	}

	hpdu = &ppdu->pdu_hdr;
	hpdu->pduh_Flags = FLAG_FINAL;
	hpdu->pduh_Opcode = IOP_NOP_Out | OP_IMMEDIATE;

	sess = conn->c_session;

	mutex_enter(&sess->s_lock);
	sn = get_sernum(sess, ppdu);
	mutex_exit(&sess->s_lock);

	if (rx_pdu != NULL) {
		hpdu->pduh_p.nop_out.TargetTransferTag =
			rx_pdu->pdu_hdr.pduh_p.nop_in.TargetTransferTag;
		hpdu->pduh_InitiatorTaskTag = rx_pdu->pdu_hdr.pduh_InitiatorTaskTag;
		hpdu->pduh_p.nop_out.CmdSN = htonl(sn);
		hpdu->pduh_LUN = rx_pdu->pdu_hdr.pduh_LUN;
	} else {
		hpdu->pduh_p.nop_out.TargetTransferTag = 0xffffffff;
		hpdu->pduh_InitiatorTaskTag = 0xffffffff;
		ccb->ccb_CmdSN = sn;
		hpdu->pduh_p.nop_out.CmdSN = htonl(sn);
	}

	DEBC(conn, 10, ("Send NOP_Out CmdSN=%d, rx_pdu=%p\n", sn, rx_pdu));

	setup_tx_uio(ppdu, 0, NULL, FALSE);
	send_pdu(ccb, ppdu, (rx_pdu != NULL) ? CCBDISP_NOWAIT : CCBDISP_FREE,
			 PDUDISP_FREE);
	return 0;
}


/*
 * snack_missing:
 *    Send SNACK request for missing data.
 *
 *    Parameter:
 *          conn     The connection
 *          ccb      The task's CCB (for Data NAK only)
 *          type     The SNACK type
 *          BegRun   The BegRun field
 *          RunLength   The RunLength field
 */

void
snack_missing(connection_t *conn, ccb_t *ccb, uint8_t type,
			  uint32_t BegRun, uint32_t RunLength)
{
	pdu_t *ppdu;
	pdu_header_t *hpdu;

	ppdu = get_pdu(conn, TRUE);
	if (ppdu == NULL)
		return;
	hpdu = &ppdu->pdu_hdr;
	hpdu->pduh_Opcode = IOP_SNACK_Request;
	hpdu->pduh_Flags = FLAG_FINAL | type;

	hpdu->pduh_InitiatorTaskTag = (type == SNACK_DATA_NAK) ? ccb->ccb_ITT : 0xffffffff;
	hpdu->pduh_p.snack.TargetTransferTag = 0xffffffff;
	hpdu->pduh_p.snack.BegRun = htonl(BegRun);
	hpdu->pduh_p.snack.RunLength = htonl(RunLength);

	ppdu->pdu_flags = PDUF_PRIORITY;

	setup_tx_uio(ppdu, 0, NULL, FALSE);
	send_pdu(NULL, ppdu, CCBDISP_NOWAIT, PDUDISP_FREE);
}


/*
 * send_snack:
 *    Send SNACK request.
 *
 *    Parameter:
 *          conn     The connection
 *          rx_pdu   The received data in PDU
 *          tx_ccb   The original command CCB (required for Data ACK only)
 *          type     The SNACK type
 *
 *    Returns:    0 on success, else an error code.
 */

void
send_snack(connection_t *conn, pdu_t *rx_pdu, ccb_t *tx_ccb, uint8_t type)
{
	pdu_t *ppdu;
	pdu_header_t *hpdu;

	ppdu = get_pdu(conn, TRUE);
	if (ppdu == NULL)
		return;
	hpdu = &ppdu->pdu_hdr;
	hpdu->pduh_Opcode = IOP_SNACK_Request;
	hpdu->pduh_Flags = FLAG_FINAL | type;

	switch (type) {
	case SNACK_DATA_NAK:
		hpdu->pduh_InitiatorTaskTag = rx_pdu->pdu_hdr.pduh_InitiatorTaskTag;
		hpdu->pduh_p.snack.TargetTransferTag = 0xffffffff;
		hpdu->pduh_p.snack.BegRun = rx_pdu->pdu_hdr.pduh_p.data_in.DataSN;
		hpdu->pduh_p.snack.RunLength = htonl(1);
		break;

	case SNACK_STATUS_NAK:
		hpdu->pduh_InitiatorTaskTag = 0xffffffff;
		hpdu->pduh_p.snack.TargetTransferTag = 0xffffffff;
		hpdu->pduh_p.snack.BegRun = rx_pdu->pdu_hdr.pduh_p.response.StatSN;
		hpdu->pduh_p.snack.RunLength = htonl(1);
		break;

	case SNACK_DATA_ACK:
		hpdu->pduh_InitiatorTaskTag = 0xffffffff;
		hpdu->pduh_p.snack.TargetTransferTag =
			rx_pdu->pdu_hdr.pduh_p.data_in.TargetTransferTag;
		hpdu->pduh_p.snack.BegRun = tx_ccb->ccb_DataSN_buf.ExpSN;
		hpdu->pduh_p.snack.RunLength = 0;
		break;

	default:
		DEBOUT(("Invalid type %d in send_snack\n", type));
		return;
	}

	hpdu->pduh_LUN = rx_pdu->pdu_hdr.pduh_LUN;

	ppdu->pdu_flags = PDUF_PRIORITY;

	setup_tx_uio(ppdu, 0, NULL, FALSE);
	send_pdu(NULL, ppdu, CCBDISP_NOWAIT, PDUDISP_FREE);
}


/*
 * send_login:
 *    Send login request.
 *
 *    Parameter:
 *          conn     The connection
 *          par      The login parameters (for negotiation)
 *
 *    Returns:       0 on success, else an error code.
 */

int
send_login(connection_t *conn)
{
	ccb_t *ccb;
	pdu_t *pdu;
	int rc;

	DEBC(conn, 9, ("Send_login\n"));
	ccb = get_ccb(conn, TRUE);
	/* only if terminating (which couldn't possibly happen here, but...) */
	if (ccb == NULL)
		return conn->c_terminating;
	pdu = get_pdu(conn, TRUE);
	if (pdu == NULL) {
		free_ccb(ccb);
		return conn->c_terminating;
	}

	if ((rc = assemble_login_parameters(conn, ccb, pdu)) <= 0) {
		init_login_pdu(conn, ccb, pdu, !rc);
		setup_tx_uio(pdu, pdu->pdu_temp_data_len, pdu->pdu_temp_data, FALSE);
		send_pdu(ccb, pdu, CCBDISP_WAIT, PDUDISP_FREE);
		rc = ccb->ccb_status;
	} else {
		free_pdu(pdu);
	}
	free_ccb(ccb);
	return rc;
}


/*
 * send_logout:
 *    Send logout request.
 *	  NOTE: This function does not wait for the logout to complete.
 *
 *    Parameter:
 *          conn	The connection
 *			refconn	The referenced connection
 *			reason	The reason code
 *			wait	Wait for completion if TRUE
 *
 *    Returns:       0 on success (logout sent), else an error code.
 */

int
send_logout(connection_t *conn, connection_t *refconn, int reason,
			bool wait)
{
	ccb_t *ccb;
	pdu_t *ppdu;
	pdu_header_t *hpdu;

	DEBC(conn, 5, ("Send_logout\n"));
	ccb = get_ccb(conn, TRUE);
	/* can only happen if terminating... */
	if (ccb == NULL)
		return conn->c_terminating;
	ppdu = get_pdu(conn, TRUE);
	if (ppdu == NULL) {
		free_ccb(ccb);
		return conn->c_terminating;
	}

	hpdu = &ppdu->pdu_hdr;
	hpdu->pduh_Opcode = IOP_Logout_Request | OP_IMMEDIATE;

	hpdu->pduh_Flags = FLAG_FINAL | reason;
	ccb->ccb_CmdSN = conn->c_session->s_CmdSN;
	hpdu->pduh_p.logout_req.CmdSN = htonl(ccb->ccb_CmdSN);
	if (reason > 0)
		hpdu->pduh_p.logout_req.CID = htons(refconn->c_id);

	ccb->ccb_par = refconn;
	if (refconn != conn) {
		ccb->ccb_flags |= CCBF_OTHERCONN;
	} else {
		conn->c_state = ST_LOGOUT_SENT;
		conn->c_loggedout = LOGOUT_SENT;
	}

	setup_tx_uio(ppdu, 0, NULL, FALSE);
	send_pdu(ccb, ppdu, (wait) ? CCBDISP_WAIT : CCBDISP_FREE, PDUDISP_FREE);

	if (wait) {
		int rc = ccb->ccb_status;
		free_ccb (ccb);
		return rc;
	}
	return 0;
}


/*
 * send_task_management:
 *    Send task management request.
 *
 *    Parameter:
 *          conn     The connection
 *          ref_ccb  The referenced command (NULL if none)
 *          xs       The scsipi command structure (NULL if not a scsipi request)
 *          function The function code
 *
 *    Returns:       0 on success, else an error code.
 */

int
send_task_management(connection_t *conn, ccb_t *ref_ccb, struct scsipi_xfer *xs,
					 int function)
{
	ccb_t *ccb;
	pdu_t *ppdu;
	pdu_header_t *hpdu;

	DEBC(conn, 5, ("Send_task_management, ref_ccb=%p, func = %d\n",
			ref_ccb, function));

	if (function == TASK_REASSIGN && conn->c_session->s_ErrorRecoveryLevel < 2)
		return ISCSI_STATUS_CANT_REASSIGN;

	ccb = get_ccb(conn, xs == NULL);
	/* can only happen if terminating... */
	if (ccb == NULL) {
		DEBC(conn, 0, ("send_task_management, ref_ccb=%p, xs=%p, term=%d. No CCB\n",
			ref_ccb, xs, conn->c_terminating));
		return conn->c_terminating;
	}
	ppdu = get_pdu(conn, xs == NULL);
	if (ppdu == NULL) {
		DEBC(conn, 0, ("send_task_management, ref_ccb=%p, xs=%p, term=%d. No PDU\n",
			ref_ccb, xs, conn->c_terminating));
		free_ccb(ccb);
		return conn->c_terminating;
	}

	ccb->ccb_xs = xs;

	hpdu = &ppdu->pdu_hdr;
	hpdu->pduh_Opcode = IOP_SCSI_Task_Management | OP_IMMEDIATE;
	hpdu->pduh_Flags = FLAG_FINAL | function;

	ccb->ccb_CmdSN = conn->c_session->s_CmdSN;
	hpdu->pduh_p.task_req.CmdSN = htonl(ccb->ccb_CmdSN);

	if (ref_ccb != NULL) {
		hpdu->pduh_p.task_req.ReferencedTaskTag = ref_ccb->ccb_ITT;
		hpdu->pduh_p.task_req.RefCmdSN = htonl(ref_ccb->ccb_CmdSN);
		hpdu->pduh_p.task_req.ExpDataSN = htonl(ref_ccb->ccb_DataSN_buf.ExpSN);
	} else
		hpdu->pduh_p.task_req.ReferencedTaskTag = 0xffffffff;

	ppdu->pdu_flags |= PDUF_PRIORITY;

	setup_tx_uio(ppdu, 0, NULL, FALSE);
	send_pdu(ccb, ppdu, (xs) ? CCBDISP_SCSIPI : CCBDISP_WAIT, PDUDISP_FREE);

	if (xs == NULL) {
		int rc = ccb->ccb_status;
		free_ccb(ccb);
		return rc;
	}
	return 0;
}


/*
 * send_data_out:
 *    Send data to target in response to an R2T or as unsolicited data.
 *
 *    Parameter:
 *          conn     The connection
 *          rx_pdu   The received R2T PDU (NULL if unsolicited)
 *          tx_ccb   The originally sent command CCB
 *          waitok   Whether it's OK to wait for an available PDU or not
 */

int
send_data_out(connection_t *conn, pdu_t *rx_pdu, ccb_t *tx_ccb,
			  ccb_disp_t disp, bool waitok)
{
	pdu_header_t *hpdu;
	uint32_t totlen, len, offs, sn;
	pdu_t *tx_pdu;

	KASSERT(conn->c_max_transfer != 0);

	if (rx_pdu) {
		offs = ntohl(rx_pdu->pdu_hdr.pduh_p.r2t.BufferOffset);
		totlen = ntohl(rx_pdu->pdu_hdr.pduh_p.r2t.DesiredDataTransferLength);
	} else {
		offs = conn->c_max_firstimmed;
		totlen = min(conn->c_max_firstdata - offs, tx_ccb->ccb_data_len - offs);
	}
	sn = 0;

	while (totlen) {
		len = min(totlen, conn->c_max_transfer);

		tx_pdu = get_pdu(conn, waitok);
		if (tx_pdu == NULL) {
			DEBC(conn, 5, ("No PDU in send_data_out\n"));

			tx_ccb->ccb_disp = disp;
			tx_ccb->ccb_status = ISCSI_STATUS_NO_RESOURCES;
			handle_connection_error(conn, ISCSI_STATUS_NO_RESOURCES, NO_LOGOUT);

			return ISCSI_STATUS_NO_RESOURCES;
		}

		totlen -= len;
		hpdu = &tx_pdu->pdu_hdr;
		hpdu->pduh_Opcode = IOP_SCSI_Data_out;
		if (!totlen)
			hpdu->pduh_Flags = FLAG_FINAL;

		if (rx_pdu != NULL)
			hpdu->pduh_p.data_out.TargetTransferTag =
				rx_pdu->pdu_hdr.pduh_p.r2t.TargetTransferTag;
		else
			hpdu->pduh_p.data_out.TargetTransferTag = 0xffffffff;
		hpdu->pduh_p.data_out.BufferOffset = htonl(offs);
		hpdu->pduh_p.data_out.DataSN = htonl(sn);

		DEBC(conn, 10, ("Send DataOut: DataSN %d, len %d offs %x totlen %d\n",
				sn, len, offs, totlen));

		setup_tx_uio(tx_pdu, len, tx_ccb->ccb_data_ptr + offs, FALSE);
		send_pdu(tx_ccb, tx_pdu, (totlen) ? CCBDISP_NOWAIT : disp, PDUDISP_FREE);

		sn++;
		offs += len;
	}
	return 0;
}


/*
 * send_command:
 *    Send a SCSI command request.
 *
 *    Parameter:
 *          CCB      The CCB
 *          disp     The CCB disposition
 */

void
send_command(ccb_t *ccb, ccb_disp_t disp, bool waitok, bool immed)
{
	uint32_t totlen, len;
	connection_t *conn = ccb->ccb_connection;
	session_t *sess = ccb->ccb_session;
	pdu_t *ppdu;
	pdu_header_t *hpdu;

	mutex_enter(&sess->s_lock);
	while (!sernum_in_window(sess)) {
		mutex_exit(&sess->s_lock);
		ccb->ccb_disp = disp;
		wake_ccb(ccb, ISCSI_STATUS_QUEUE_FULL);
		return;
	}
	mutex_exit(&sess->s_lock);

	/* Don't confuse targets during (re-)negotations */
	if (conn->c_state != ST_FULL_FEATURE) {
		DEBOUT(("Invalid connection for send_command, ccb = %p\n",ccb));
		ccb->ccb_disp = disp;
		wake_ccb(ccb, ISCSI_STATUS_TARGET_BUSY);
		return;
	}

	ppdu = get_pdu(conn, waitok);
	if (ppdu == NULL) {
		DEBOUT(("No PDU for send_command, ccb = %p\n",ccb));
		ccb->ccb_disp = disp;
		wake_ccb(ccb, ISCSI_STATUS_NO_RESOURCES);
		return;
	}

	totlen = len = ccb->ccb_data_len;

	hpdu = &ppdu->pdu_hdr;
	hpdu->pduh_LUN = htonq(ccb->ccb_lun);
	memcpy(hpdu->pduh_p.command.SCSI_CDB, ccb->ccb_cmd, ccb->ccb_cmdlen);
	hpdu->pduh_Opcode = IOP_SCSI_Command;
	if (immed)
		hpdu->pduh_Opcode |= OP_IMMEDIATE;
	hpdu->pduh_p.command.ExpectedDataTransferLength = htonl(totlen);

	if (totlen) {
		if (ccb->ccb_data_in) {
			hpdu->pduh_Flags = FLAG_READ;
			totlen = 0;
		} else {
			hpdu->pduh_Flags = FLAG_WRITE;
			/* immediate data we can send */
			len = min(totlen, conn->c_max_firstimmed);

			/* can we send more unsolicited data ? */
			totlen = conn->c_max_firstdata ? totlen - len : 0;
		}
	}
	if (!totlen)
		hpdu->pduh_Flags |= FLAG_FINAL;
	hpdu->pduh_Flags |= ccb->ccb_tag;

	if (ccb->ccb_data_in)
		init_sernum(&ccb->ccb_DataSN_buf);

	ccb->ccb_sense_len_got = 0;
	ccb->ccb_xfer_len = 0;
	ccb->ccb_residual = 0;
	ccb->ccb_flags |= CCBF_REASSIGN;

	mutex_enter(&sess->s_lock);
	ccb->ccb_CmdSN = get_sernum(sess, ppdu);
	mutex_exit(&sess->s_lock);

	hpdu->pduh_p.command.CmdSN = htonl(ccb->ccb_CmdSN);

	DEBC(conn, 10, ("Send Command: CmdSN %d (%d), data_in %d, len %d, totlen %d\n",
			ccb->ccb_CmdSN, sess->s_MaxCmdSN, ccb->ccb_data_in, len, totlen));

	setup_tx_uio(ppdu, len, ccb->ccb_data_ptr, ccb->ccb_data_in);
	send_pdu(ccb, ppdu, (totlen) ? CCBDISP_DEFER : disp, PDUDISP_WAIT);

	if (totlen)
		send_data_out(conn, NULL, ccb, disp, waitok);
}


/*
 * send_run_xfer:
 *    Handle a SCSI command transfer request from scsipi.
 *
 *    Parameter:
 *          session  The session
 *          xs       The transfer parameters
 */

void
send_run_xfer(session_t *session, struct scsipi_xfer *xs)
{
	ccb_t *ccb;
	connection_t *conn;
	bool waitok;

	waitok = !(xs->xs_control & XS_CTL_NOSLEEP);

	DEB(10, ("RunXfer: flags=%x, data=%p, datalen=%d, resid=%d, cmdlen=%d, "
			"waitok=%d\n", xs->xs_control, xs->data, xs->datalen,
			xs->resid, xs->cmdlen, waitok));

	conn = assign_connection(session, waitok);

	if (conn == NULL || conn->c_terminating || conn->c_state != ST_FULL_FEATURE) {
		xs->error = XS_SELTIMEOUT;
		DEBC(conn, 10, ("run_xfer on dead connection\n"));
		scsipi_done(xs);
		unref_session(session);
		return;
	}

	if (xs->xs_control & XS_CTL_RESET) {
		if (send_task_management(conn, NULL, xs, TARGET_WARM_RESET)) {
			DEBC(conn, 0, ("send_task_management TARGET_WARM_RESET failed\n"));
			xs->error = XS_SELTIMEOUT;
			scsipi_done(xs);
			unref_session(session);
		}
		return;
	}

	ccb = get_ccb(conn, waitok);
	if (ccb == NULL) {
		xs->error = XS_BUSY;
		DEBC(conn, 5, ("No CCB in run_xfer, %d in use.\n", conn->c_usecount));
		scsipi_done(xs);
		unref_session(session);
		return;
	}
	/* copy parameters into CCB for easier access */
	ccb->ccb_xs = xs;

	ccb->ccb_data_in = (xs->xs_control & XS_CTL_DATA_IN) != 0;
	ccb->ccb_data_len = (uint32_t) xs->datalen;
	ccb->ccb_data_ptr = xs->data;

	ccb->ccb_sense_len_req = sizeof(xs->sense.scsi_sense);
	ccb->ccb_sense_ptr = &xs->sense;

	ccb->ccb_lun = ((uint64_t) (uint8_t) xs->xs_periph->periph_lun) << 48;
	ccb->ccb_cmd = (uint8_t *) xs->cmd;
	ccb->ccb_cmdlen = xs->cmdlen;
	DEB(10, ("RunXfer: Periph_lun = %d, cmd[1] = %x, cmdlen = %d\n",
			xs->xs_periph->periph_lun, ccb->ccb_cmd[1], xs->cmdlen));

	ccb->ccb_ITT |= xs->xs_tag_id << 24;
	switch (xs->xs_tag_type) {
	case MSG_ORDERED_Q_TAG:
		ccb->ccb_tag = ATTR_ORDERED;
		break;
	case MSG_SIMPLE_Q_TAG:
		ccb->ccb_tag = ATTR_SIMPLE;
		break;
	case MSG_HEAD_OF_Q_TAG:
		ccb->ccb_tag = ATTR_HEAD_OF_QUEUE;
		break;
	default:
		ccb->ccb_tag = 0;
		break;
	}

#ifdef LUN_1
	ccb->ccb_lun += 0x1000000000000LL;
	ccb->ccb_cmd[1] += 0x10;
#endif
	send_command(ccb, CCBDISP_SCSIPI, waitok, FALSE);
}


#ifndef ISCSI_MINIMAL
/*
 * send_io_command:
 *    Handle a SCSI io command request from user space.
 *
 *    Parameter:
 *          session 	The session
 *          lun		    The LUN to use
 *          req			The SCSI request block
 *			immed		Immediate command if TRUE
 *			conn_id		Assign to this connection ID if nonzero
 */

int
send_io_command(session_t *session, uint64_t lun, scsireq_t *req,
				bool immed, uint32_t conn_id)
{
	ccb_t *ccb;
	connection_t *conn;
	int rc;

	DEB(9, ("IoCommand: lun=%x, datalen=%d, cmdlen=%d, immed=%d, cid=%d\n",
			(int) lun, (int) req->datalen, (int) req->cmdlen, immed, conn_id));

	conn = (conn_id) ? find_connection(session, conn_id)
					 : assign_connection(session, TRUE);

	if (conn == NULL || conn->c_terminating || conn->c_state != ST_FULL_FEATURE) {
		DEBOUT(("io_command on dead connection (state = %d)\n",
				(conn != NULL) ? conn->c_state : -1));
		return ISCSI_STATUS_INVALID_CONNECTION_ID;
	}

	ccb = get_ccb(conn, TRUE);
	if (ccb == NULL) {
		DEBOUT(("No CCB in io_command\n"));
		return ISCSI_STATUS_NO_RESOURCES;
	}

	ccb->ccb_data_in = (req->flags & SCCMD_READ) != 0;
	ccb->ccb_data_len = (uint32_t) req->datalen;
	ccb->ccb_data_ptr = req->databuf;

	ccb->ccb_sense_len_req = req->senselen;
	ccb->ccb_sense_ptr = &req->sense;

	ccb->ccb_lun = lun;
	ccb->ccb_cmd = (uint8_t *) req->cmd;
	ccb->ccb_cmdlen = req->cmdlen;
	DEBC(conn, 10, ("IoCommand: cmd[1] = %x, cmdlen = %d\n",
			 ccb->ccb_cmd[1], ccb->ccb_cmdlen));

	send_command(ccb, CCBDISP_WAIT, TRUE, immed);

	rc = ccb->ccb_status;

	req->senselen_used = ccb->ccb_sense_len_got;
	req->datalen_used = req->datalen - ccb->ccb_residual;

	free_ccb(ccb);

	return rc;
}
#endif


/*****************************************************************************
 * Timeout handlers
 *****************************************************************************/
/*
 * connection_timeout:
 *    Handle prolonged silence on a connection by checking whether
 *    it's still alive.
 *    This has the side effect of discovering missing status or lost commands
 *    before those time out.
 *
 *    Parameter:
 *          conn     The connection
 */

void
connection_timeout(connection_t *conn)
{

	if (++conn->c_num_timeouts > MAX_CONN_TIMEOUTS)
		handle_connection_error(conn, ISCSI_STATUS_TIMEOUT, NO_LOGOUT);
	else {
		if (conn->c_state == ST_FULL_FEATURE)
			send_nop_out(conn, NULL);

		connection_timeout_start(conn, CONNECTION_TIMEOUT);
	}
}

/*
 * ccb_timeout:
 *    Handle timeout of a sent command.
 *
 *    Parameter:
 *          ccb      The CCB
 */

void
ccb_timeout(ccb_t *ccb)
{
	connection_t *conn = ccb->ccb_connection;

	ccb->ccb_total_tries++;

	DEBC(conn, 0, ("ccb_timeout: num=%d total=%d disp=%d\n",
		ccb->ccb_num_timeouts+1, ccb->ccb_total_tries, ccb->ccb_disp));

	if (++ccb->ccb_num_timeouts > MAX_CCB_TIMEOUTS ||
		ccb->ccb_total_tries > MAX_CCB_TRIES ||
		ccb->ccb_disp <= CCBDISP_FREE ||
		!ccb->ccb_session->s_ErrorRecoveryLevel) {

		wake_ccb(ccb, ISCSI_STATUS_TIMEOUT);
		handle_connection_error(conn, ISCSI_STATUS_TIMEOUT, RECOVER_CONNECTION);
	} else {
		if (ccb->ccb_data_in && ccb->ccb_xfer_len < ccb->ccb_data_len) {
			/* request resend of all missing data */
			snack_missing(conn, ccb, SNACK_DATA_NAK, 0, 0);
		} else {
			/* request resend of all missing status */
			snack_missing(conn, NULL, SNACK_STATUS_NAK, 0, 0);
		}
		ccb_timeout_start(ccb, COMMAND_TIMEOUT);
	}
}

