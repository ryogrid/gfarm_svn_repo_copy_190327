# $Id: Makefile 9308 2014-12-09 04:12:58Z m-kasahr $

top_builddir = ../../../..
top_srcdir = $(top_builddir)
srcdir = .

include $(top_srcdir)/makes/var.mk

SUBDIRS=debian default redhat suse systemd

datadir=$(default_datadir)/gfarm/config

DATA =	$(srcdir)/config-gfarm.sysdep

include $(top_srcdir)/makes/data.mk
include $(top_srcdir)/makes/subdir.mk
