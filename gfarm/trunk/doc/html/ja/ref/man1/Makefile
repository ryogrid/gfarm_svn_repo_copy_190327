# $Id: Makefile 1265 2004-08-17 01:47:35Z soda $

top_builddir = ../../../../..
top_srcdir = $(top_builddir)
srcdir = .

include $(top_srcdir)/makes/var.mk

htmldir=$(default_htmldir)/ja/ref/man1

DOCBOOK_DIR=$(top_srcdir)/doc/docbook/ja/ref/man1
include $(DOCBOOK_DIR)/Makefile.inc
HTMLSRC=$(DOCBOOK)

include $(top_srcdir)/makes/html.mk
