# $Id: Makefile 10212 2016-09-02 05:52:24Z tatebe $

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
