# $Id: Makefile 3468 2007-01-15 09:54:24Z n-soda $

top_builddir = ../..
top_srcdir = $(top_builddir)
srcdir = .

include $(top_srcdir)/makes/var.mk

SUBDIRS= sysdep/$(config_gfarm_sysdep_subdir)

datadir=$(default_datadir)/gfarm/config

SCRIPTS = config-gfarm config-agent config-gfsd $(srcdir)/gfarm.arch.guess
DATA =	$(srcdir)/config-gfarm.postgresql \
	$(srcdir)/config-gfarm.ldap \
	$(srcdir)/gfarm.conf-postgresql.in \
	$(srcdir)/gfarm.conf-ldap.in \
	$(srcdir)/slapd.conf-2.0.in \
	$(srcdir)/slapd.conf-2.1.in \
	$(srcdir)/gfarm.schema-1.sql \
	$(srcdir)/gfarm.schema-2.sql \
	$(srcdir)/gfarm.schema \
	$(srcdir)/initial.ldif.in \
	$(srcdir)/bdb.DB_CONFIG.in

include $(top_srcdir)/makes/script.mk
include $(top_srcdir)/makes/data.mk
include $(top_srcdir)/makes/subdir.mk
