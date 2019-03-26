# $Id: Makefile 70 2002-02-02 02:32:10Z soda $

top_objdir = ../../..
top_srcdir = $(top_objdir)
srcdir = .

include $(top_srcdir)/makes/var.mk
include $(srcdir)/../Makefile.inc

SRCS = gfimport_text.c
OBJS = gfimport_text.o
PROGRAM = gfimport_text
CFLAGS = $(COMMON_CFLAGS)
LDLIBS = $(COMMON_LDLIBS) $(GFARMLIB) $(LIBS)
DEPLIBS = $(DEPGFARMLIB)

all: $(PROGRAM)

include $(top_srcdir)/makes/prog.mk

###

$(OBJS): $(DEPGFARMINC)
