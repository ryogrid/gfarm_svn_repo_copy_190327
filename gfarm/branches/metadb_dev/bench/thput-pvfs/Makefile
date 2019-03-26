# $Id: Makefile 95 2002-04-19 07:27:52Z tatebe $

top_objdir = ../..
top_srcdir = $(top_objdir)
srcdir = .

include $(top_srcdir)/makes/var.mk

CFLAGS = $(COMMON_CFLAGS) -I/usr/local/include -DPVFS_MOUNT=\"/mnt/pvfs\"
LDLIBS = $(COMMON_LDFLAGS) $(LIBS) -L/usr/local/lib -lpvfs

PROGRAM = thput-pvfs
OBJS = thput-pvfs.o

all: $(PROGRAM)

include $(top_srcdir)/makes/prog.mk
