# $Id: Makefile 70 2002-02-02 02:32:10Z soda $

top_objdir = ../..
top_srcdir = $(top_objdir)
srcdir = .

include $(top_srcdir)/makes/var.mk

CFLAGS = $(COMMON_CFLAGS)
LDLIBS = $(COMMON_LDFLAGS) $(LIBS)

PROGRAM = thput-fsys
OBJS = thput-fsys.o

all: $(PROGRAM)

include $(top_srcdir)/makes/prog.mk
