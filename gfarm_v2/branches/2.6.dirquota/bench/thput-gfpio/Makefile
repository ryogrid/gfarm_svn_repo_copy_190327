# $Id: Makefile 10268 2016-11-10 08:59:43Z n-soda $

top_builddir = ../..
top_srcdir = $(top_builddir)
srcdir = .

include $(top_srcdir)/makes/var.mk

CFLAGS = $(COMMON_CFLAGS)
LDLIBS = $(COMMON_LDFLAGS) $(GFARMLIB) $(LIBS)
DEPLIBS = $(DEPGFARMLIB)

PROGRAM = thput-gfpio
OBJS = thput-gfpio.o

all: $(PROGRAM)

include $(top_srcdir)/makes/prog.mk
