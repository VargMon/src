#	$NetBSD: Makefile,v 1.4 2018/02/04 01:16:33 mrg Exp $

UNSUPPORTED_COMPILER.clang=	# defined

.include <bsd.own.mk>

.include "../Makefile.inc"
.include "../Makefile.sanitizer"

.PATH: ${SANITIZER}/ubsan

UBSAN_SRCS= \
	ubsan_diag.o \
	ubsan_flags.o \
	ubsan_handlers.o \
	ubsan_handlers_cxx.o \
	ubsan_init.o \
	ubsan_init_standalone.o \
	ubsan_type_hash.o \
	ubsan_type_hash_itanium.o \
	ubsan_type_hash_win.o \
	ubsan_value.o


.for _s in ubsan_handlers_cxx ubsan_type_hash ubsan_type_hash_itanium
COPTS.${_s}.cc+=-frtti
.endfor

LIB=	ubsan
SRCS+=	${UBSAN_SRCS}

.if ${MACHINE_ARCH} == "vax"
COPTS += -O1
.endif

.include <bsd.lib.mk>
