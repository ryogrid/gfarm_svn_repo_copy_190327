# $Id: Makefile 1265 2004-08-17 01:47:35Z soda $

top_builddir = ../../..
top_srcdir = $(top_builddir)
srcdir = .

include $(top_srcdir)/makes/var.mk
include $(srcdir)/../Makefile.inc

DOCBOOK_DIR=$(top_srcdir)/doc/docbook/ja/ref/man3
include $(DOCBOOK_DIR)/Makefile.inc
MAN = $(DOCBOOK)
EXTRA_CLEAN_TARGETS = manpage.links manpage.refs

include $(top_srcdir)/makes/man.mk
