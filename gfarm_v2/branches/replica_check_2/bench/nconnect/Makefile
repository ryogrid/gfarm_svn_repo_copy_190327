# $Id: Makefile 2373 2006-01-12 00:54:22Z soda $

top_builddir = ../..
top_srcdir = $(top_builddir)
srcdir = .

include $(top_srcdir)/makes/var.mk

CFLAGS = $(COMMON_CFLAGS)
LDLIBS = $(COMMON_LDFLAGS) $(LIBS)

PROGRAM = nconnect
OBJS = nconnect.o

all: $(PROGRAM)

include $(top_srcdir)/makes/prog.mk
