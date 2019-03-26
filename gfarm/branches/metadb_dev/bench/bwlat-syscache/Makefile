# $Id: Makefile 105 2002-05-10 03:13:36Z soda $

top_objdir = ../..
top_srcdir = $(top_objdir)
srcdir = .

include $(top_srcdir)/makes/var.mk

CFLAGS = $(COMMON_CFLAGS)
LDLIBS = $(COMMON_LDFLAGS) $(LIBS)

PROGRAM = bwlat-syscache
OBJS = bwlat-syscache.o

all: $(PROGRAM)

include $(top_srcdir)/makes/prog.mk
