/*
 * Copyright (c) 2003-2006 National Institute of Advanced
 * Industrial Science and Technology (AIST).  All rights reserved.
 *
 * Copyright (c) 2006 National Institute of Informatics in Japan,
 * All rights reserved.
 *
 * This file or a portion of this file is licensed under the terms of
 * the NAREGI Public License, found at
 * http://www.naregi.org/download/index.html.
 * If you redistribute this file, with or without modifications, you
 * must include this notice in the file.
 */

/*
 * $Id: metadb_pgsql.c 3725 2007-05-11 12:46:50Z tatebe $
 */

#include <assert.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <libpq-fe.h>
#include <netinet/in.h>
#include <gfarm/gfarm.h>

#include "gfutil.h"

#include "config.h"
#include "metadb_access.h"
#include "metadb_sw.h"

/* ERROR:  null value in column "...." violates not-null constraint */
#define GFARM_PGSQL_ERRCODE_NOT_NULL_VIOLATION	"23502"

#define GFARM_PGSQL_ERRCODE_UNIQUE_VIOLATION	"23505"

/* FATAL:  terminating connection due to administrator command\n
   could not receive data from server: Bad file number */
#define GFARM_PGSQL_ERRCODE_ADMIN_SHUTDOWN	"57P01"
/**********************************************************************/

static PGconn *conn = NULL;

static int use_hostid_and_pathid; /* is schema version 2 ? */

#define	PGSQL_MSG_LEN 1024

static char *
save_pgsql_msg(char *s)
{
	static char msg[PGSQL_MSG_LEN + 1];
	int len = strlen(s);

	if (len >= sizeof(msg))
		len = sizeof(msg) - 1;
	memcpy(msg, s, len);
	msg[len] = '\0';
	if (len > 0 && msg[len - 1] == '\n')
		msg[len - 1] = '\0';
	return(msg);
}

/*
 * remark:
 * 1. This function returns NULL if the caller should retry, not NULL if it
 *    should  end because of unrecoverable error.
 * 2. This function calls PQclear() if the caller retries, otherwise does not.
 */
static char *
can_I_retry(PGresult *res, int *fatal_cntp)
{
	char *e;

	if (PQstatus(conn) == CONNECTION_BAD && (*fatal_cntp)++ <= 1) {
		PQreset(conn);
		if (PQstatus(conn) == CONNECTION_OK) {
			PQclear(res);
			return (NULL);
		}
	} else if (PQresultStatus(res) == PGRES_FATAL_ERROR &&
		   strcmp(PQresultErrorField(res, PG_DIAG_SQLSTATE),
			GFARM_PGSQL_ERRCODE_ADMIN_SHUTDOWN) == 0 &&
		   (*fatal_cntp)++ == 0) {
		PQclear(res);
		return (NULL);
	}
	e = save_pgsql_msg(PQresultErrorMessage(res));
	return (e);
}

static char *
gfarm_pgsql_make_conninfo(const char **varnames, char **varvalues, int n,
	char *others)
{
	int i;
	size_t length = 0;
	char *v, *conninfo, *p;

	/* count necessary string length */
	for (i = 0; i < n; i++) {
		if ((v = varvalues[i]) != NULL) {
			if (i > 1)
				length++; /* space */
			length += strlen(varnames[i]) + 3; /* var='' */
			for (; *v != '\0'; v++) {
				if (*v == '\'' || *v == '\\')
					++length;
				++length;
			}
		}
	}
	if (others != NULL) {
		if (length > 0)
			++length; /* space */
		length += strlen(others);
	}
	++length; /* '\0' */

	GFARM_MALLOC_ARRAY(conninfo, length);
	if (conninfo == NULL)
		return (NULL);

	p = conninfo;
	for (i = 0; i < n; i++) {
		if ((v = varvalues[i]) != NULL) {
			if (i > 1)
				*p++ = ' ';
			p += sprintf(p, "%s='", varnames[i]);
			for (; *v != '\0'; v++) {
				if (*v == '\'' || *v == '\\')
					*p++ = '\\';
				*p++ = *v;
			}
			*p++ = '\'';
		}
	}
	if (others != NULL) {
		if (p > conninfo)
			*p++ = ' ';
		p += sprintf(p, "%s", others);
	}
	*p++ = '\0';
	return (conninfo);
}

static char *
gfarm_pgsql_initialize(void)
{
	int port;
	static const char *varnames[] = {
		"host", "port", "dbname", "user", "password"
	};
	char *varvalues[GFARM_ARRAY_LENGTH(varnames)];
	char *e, *conninfo;

	/*
	 * sanity check:
	 * all parameters can be NULL,
	 * in such cases, enviroment variables will be used.
	 */
	if (gfarm_postgresql_server_port != NULL) {
		port = strtol(gfarm_postgresql_server_port, &e, 0);
		if (e == gfarm_postgresql_server_port ||
		    port <= 0 || port >= 65536)
			return ("gfarm.conf: postgresql_serverport: "
			    "illegal value");
	}

	varvalues[0] = gfarm_postgresql_server_name;
	varvalues[1] = gfarm_postgresql_server_port;
	varvalues[2] = gfarm_postgresql_dbname;
	varvalues[3] = gfarm_postgresql_user;
	varvalues[4] = gfarm_postgresql_password;
	conninfo = gfarm_pgsql_make_conninfo(
	    varnames, varvalues, GFARM_ARRAY_LENGTH(varnames),
	    gfarm_postgresql_conninfo);
	if (conninfo == NULL)
		return (GFARM_ERR_NO_MEMORY);

	/*
	 * initialize PostgreSQL
	 */

	/* open a connection */
	conn = PQconnectdb(conninfo);
	free(conninfo);

	e = NULL;
	if (PQstatus(conn) != CONNECTION_OK) {
		/* PQerrorMessage's return value will be freed in PQfinish() */
		e = save_pgsql_msg(PQerrorMessage(conn));
		(void)gfarm_metadb_terminate();
	} else { /* determine the version of the schema */
		PGresult *res;

		res = PQexecParams(conn,
		    "SELECT COUNT(*) FROM Host_hostid_seq, Path_pathid_seq",
		    0, /* number of params */
		    NULL, /* param types */
		    NULL, /* params */
		    NULL, /* param lengths */
		    NULL, /* param formats */
		    0); /* dummy parameter for result format */
		use_hostid_and_pathid =
		    PQresultStatus(res) == PGRES_TUPLES_OK;
		PQclear(res);
	}
	return (e);
}

static char *
gfarm_pgsql_terminate(void)
{
	/* close and free connection resources */
	if (gfarm_does_own_metadb_connection()) {
		PQfinish(conn);
	}
	conn = NULL;

	return (NULL);
}

/*
 * PostgreSQL connection cannot be used from forked children unless
 * the connection is ensured to be used exclusively.
 * This routine guarantees that never happens.
 * NOTE:
 * This is where gfarm_metadb_initialize() is called from.
 * Nearly every interface functions must call this function.
 */
static char *
gfarm_pgsql_check(void)
{
	char *e;
	static int error_is_reported = 0;

	/*
	 * if there is a valid PostgreSQL connection, return.  If not,
	 * create a new connection.
	 */
	if (conn != NULL && gfarm_does_own_metadb_connection()) {
		error_is_reported = 0;
		return (NULL);
	}
	/* XXX close the file descriptor for conn, but how? */
	conn = NULL;
	e = gfarm_metadb_initialize();
	if (e == NULL) {
		error_is_reported = 0;
	} else if (!error_is_reported && gflog_syslog_enabled()) {
		error_is_reported = 1;
		gflog_error("gfarm_pgsql_initialize: %s", e);
	}
	return (e);
}

/**********************************************************************/

uint64_t
gfarm_hton64(uint64_t h64)
{
#ifdef WORDS_BIGENDIAN
	return (h64); /* same order */
#else
	return (htonl((uint32_t)((h64 >> 32) & 0xffffffff)) |
	    ((uint64_t)htonl((uint32_t)h64 & 0xffffffff) << 32));
#endif
}

uint64_t
gfarm_ntoh64(uint64_t n64)
{
	return (gfarm_hton64(n64));
}

static int64_t
pgsql_get_int64(PGresult *res, int row, const char *field_name)
{
	uint64_t val;

	memcpy(&val, PQgetvalue(res, row, PQfnumber(res, field_name)),
	    sizeof(val));
	return (gfarm_ntoh64(val));
}

static int32_t
pgsql_get_int32(PGresult *res, int row, const char *field_name)
{
	uint32_t val;

	memcpy(&val, PQgetvalue(res, row, PQfnumber(res, field_name)),
	    sizeof(val));
	return (ntohl(val));
}

static char *
pgsql_get_string(PGresult *res, int row, const char *field_name)
{
	return (strdup(PQgetvalue(res, row, PQfnumber(res, field_name))));
}

/**********************************************************************/

static char *
host_info_get_one(
	PGresult *res,
	int startrow,
	int nhostaliases,
	struct gfarm_host_info *info)
{
	int i;

	info->hostname = pgsql_get_string(res, startrow, "hostname");
	info->architecture = pgsql_get_string(res, startrow, "architecture");
	info->ncpu = pgsql_get_int32(res, startrow, "ncpu");
	info->nhostaliases = nhostaliases;
	GFARM_MALLOC_ARRAY(info->hostaliases, nhostaliases + 1);
	for (i = 0; i < nhostaliases; i++) {
		info->hostaliases[i] = pgsql_get_string(res, startrow + i,
		    "hostalias");
	}
	info->hostaliases[info->nhostaliases] = NULL;
	return (NULL);
}

static char *
host_info_get(
	char *csql,
	char *isql,
	int nparams,
	const char **paramValues,
	struct gfarm_host_info *info)
{
	PGresult *res, *resi, *resc;
	char *e = NULL;
	int fatal_cnt = 0;

 retry:
	res = PQexec(conn, "BEGIN");

	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
		PQclear(res);
		goto end;
	}
	PQclear(res);

	resc = PQexecParams(conn,
		csql,
		nparams,
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		1); /* ask for binary results */
	if (PQresultStatus(resc) != PGRES_TUPLES_OK) {
		e = save_pgsql_msg(PQresultErrorMessage(res));
		goto clear_resc;
	}

	resi = PQexecParams(conn,
		isql,
		nparams, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		1); /* ask for binary results */
	if (PQresultStatus(resi) != PGRES_TUPLES_OK) {
		e = save_pgsql_msg((PQresultErrorMessage(res)));
		goto clear_resi;
	}
	if (PQntuples(resi) == 0) {
		e = GFARM_ERR_NO_SUCH_OBJECT;
		goto clear_resi;
	}

	gfarm_base_host_info_ops.clear(info);
	e = host_info_get_one(resi, 0,
	    pgsql_get_int64(resc, 0, "count"), info);

 clear_resi:
	PQclear(resi);
 clear_resc:
	PQclear(resc);
 end:
	res = PQexec(conn, "END");
	PQclear(res);

	return (e);
}

static char *
gfarm_pgsql_host_info_get(
	const char *hostname,
	struct gfarm_host_info *info)
{
	char *e;
	const char *paramValues[1];

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

	paramValues[0] = hostname;
	return (host_info_get(
		"SELECT count(hostaliases) "
		    "FROM HostAliases WHERE hostname = $1",

		"SELECT Host.hostname, architecture, ncpu, hostalias "
		    "FROM Host LEFT OUTER JOIN HostAliases "
			"ON Host.hostname = HostAliases.hostname "
		"WHERE Host.hostname = $1 AND Host.available"
		"ORDER BY Host.hostname, hostalias",

		1,
		paramValues,
		info));
}

static char *
hostaliases_remove(const char *hostname)
{
	PGresult *res;
	const char *paramValues[1];
	char *e = NULL;
	int fatal_cnt = 0;

 retry:
	paramValues[0] = hostname;
	res = PQexecParams(conn,
		"DELETE FROM HostAliases WHERE hostname = $1",
		1, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0); /* dummy parameter for result format */

	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
	}
	PQclear(res);
	return (e);
}

static char *
gfarm_pgsql_host_info_remove_hostaliases(const char *hostname)
{
	char *e;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);
	/*
	 * XXX - needs to check if hostname exists in Hosts.
	 *       this check and deletion should be done in a transaction
	 */

	return (hostaliases_remove(hostname));
}

static char *
hostaliases_set(
	const char *hostname,
	const struct gfarm_host_info *info)
{
	PGresult *res;
	const char *paramValues[2];
	int i;
	char *e = NULL;

	if (info->hostaliases != NULL) {
		for (i = 0; i < info->nhostaliases; i++) {
			paramValues[0] = hostname;
			paramValues[1] = info->hostaliases[i];
			res = PQexecParams(conn,
				"INSERT INTO Hostaliases (hostname, hostalias)"
				    " VALUES ($1, $2)",
				2, /* number of params */
				NULL, /* param types */
				paramValues,
				NULL, /* param lengths */
				NULL, /* param formats */
				0); /* dummy parameter for result format */
			if (PQresultStatus(res) != PGRES_COMMAND_OK) {
				e = save_pgsql_msg(
						PQresultErrorMessage(res));
				PQclear(res);
				/* XXX */
				if (strstr(
				    e,
				    "duplicate key violates unique constraint")
				    != NULL) {
					e = GFARM_ERR_ALREADY_EXISTS;
				}
				return (e);
			}
			PQclear(res);
		}
	}
	return (e);
}

static char *
gfarm_pgsql_host_info_set(
	const char *hostname,
	const struct gfarm_host_info *info)
{
	PGresult *res;
	const char *paramValues[4];
	char *e;
	char ncpu[GFARM_INT32STRLEN + 1];
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

 retry:
	res = PQexec(conn, "BEGIN");
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
		PQclear(res);
		goto end;
	}
	PQclear(res);

	paramValues[1] = info->architecture;
	sprintf(ncpu, "%d", info->ncpu);
	paramValues[2] = ncpu;

	if (use_hostid_and_pathid) {
		/* see if the hostname once existed */
		char *old_hostname;

		GFARM_MALLOC_ARRAY(old_hostname, strlen(hostname) + 1 + 1);
		if (old_hostname == NULL) {
			e = GFARM_ERR_NO_MEMORY;
			goto end;
		}
		sprintf(old_hostname, "#%s", hostname);

		paramValues[0] = old_hostname;
		res = PQexecParams(conn,
			"SELECT TRUE FROM Host where hostname = $1",
			1, /* number of params */
			NULL, /* param types */
			paramValues,
			NULL, /* param lengths */
			NULL, /* param formats */
			1); /* ask for binary results */

		if (PQresultStatus(res) == PGRES_TUPLES_OK &&
		    PQntuples(res) == 1) {
			PQclear(res);

			paramValues[3] = hostname;
			res = PQexecParams(conn,
				"UPDATE Host SET architecture = $2, ncpu = $3,"
					"hostname = $4, available = TRUE "
				    "WHERE hostname = $1",
				4, /* number of params */
				NULL, /* param types */
				paramValues,
				NULL, /* param lengths */
				NULL, /* param formats */
				0); /* dummy parameter for result format */
			if (PQresultStatus(res) == PGRES_COMMAND_OK &&
			    strtol(PQcmdTuples(res), NULL, 0) > 0) {
				PQclear(res);
				free(old_hostname);
				e = NULL;
				goto added;
			}
		}
		PQclear(res);
		free(old_hostname);
	}

	paramValues[0] = hostname;
	res = PQexecParams(conn,
		!use_hostid_and_pathid ?
		"INSERT INTO Host (hostname, architecture, ncpu) "
		    "VALUES ($1, $2, $3)" :
		"INSERT INTO Host (hostname, architecture, ncpu, available) "
		    "VALUES ($1, $2, $3, TRUE)",
		3, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0); /* dummy parameter for result format */
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = save_pgsql_msg(PQresultErrorMessage(res));
		PQclear(res);
		/* XXX */
		if (strstr(
		    e, "duplicate key violates unique constraint") != NULL) {
			e = GFARM_ERR_ALREADY_EXISTS;
		}
		goto end;
	}
	PQclear(res);

 added:
	e = hostaliases_set(hostname, info);
 end:
	if (e == NULL)
		res = PQexec(conn, "COMMIT");
	else
		res = PQexec(conn, "ROLLBACK");
	PQclear(res);

	return (e);
}

/*
 * NOTE: the first character of the hostname parameter must be '#'.
 * this function is only called when `use_hostid_and_pathid'.
 */
static int
removed_host_info_set(const char *hostname)
{
	int ok;
	PGresult *res;
	const char *paramValues[1];

	paramValues[0] = hostname;
	res = PQexecParams(conn,
		"INSERT INTO Host (hostname, architecture, ncpu, available) "
		    "VALUES ($1, 'unknown', 1, FALSE)",
		1, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0); /* dummy parameter for result format */
	ok = PQresultStatus(res) == PGRES_COMMAND_OK;
	PQclear(res);
	return (ok);
}

static char *
gfarm_pgsql_host_info_replace(
	const char *hostname,
	const struct gfarm_host_info *info)
{
	PGresult *res;
	const char *paramValues[3];
	char *e;
	char ncpu[GFARM_INT32STRLEN + 1];
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

 retry:
	res = PQexec(conn, "BEGIN");
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
		PQclear(res);
		goto end;
	}
	PQclear(res);

	paramValues[0] = hostname;
	paramValues[1] = info->architecture;
	sprintf(ncpu, "%d", info->ncpu);
	paramValues[2] = ncpu;
	res = PQexecParams(conn,
		"UPDATE Host SET architecture = $2, ncpu = $3 "
		    "WHERE hostname = $1",
		3, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0); /* dummy parameter for result format */
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = save_pgsql_msg(PQresultErrorMessage(res));
		PQclear(res);
		goto end;
	}
	if (strtol(PQcmdTuples(res), NULL, 0) == 0) {
		e = GFARM_ERR_NO_SUCH_OBJECT;
		PQclear(res);
		goto end;
	}
	PQclear(res);

	e = hostaliases_remove(hostname);
	if (e != NULL)
		goto end;
	e = hostaliases_set(hostname, info);
 end:
	if (e == NULL)
		res = PQexec(conn, "COMMIT");
	else
		res = PQexec(conn, "ROLLBACK");
	PQclear(res);

	return (e);
}

static char *
gfarm_pgsql_host_info_remove(const char *hostname)
{
	PGresult *res;
	const char *paramValues[2];
	char *e;
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

 retry:
	if (!use_hostid_and_pathid) {
		paramValues[0] = hostname;
		res = PQexecParams(conn,
			"DELETE FROM Host WHERE hostname = $1",
			1, /* number of params */
			NULL, /* param types */
			paramValues,
			NULL, /* param lengths */
			NULL, /* param formats */
			0);  /* dummy parameter for result format */

		if (PQresultStatus(res) != PGRES_COMMAND_OK) {
			e = can_I_retry(res, &fatal_cnt);
			if (e == NULL)
				goto retry;
		} else if (strtol(PQcmdTuples(res), NULL, 0) == 0)
			e = GFARM_ERR_NO_SUCH_OBJECT;
		PQclear(res);
	} else {
		/*
		 * To prevent dangling reference from a FileSectionCopy,
		 * we do not remove a Host, but make it unavailable.
		 *
		 * We also rename the hostname to "#hostname",
		 * so that replicas which refer this host become
		 * inaccessible.
		 * XXX is this really good idea?
		 */
		char *new_hostname = NULL;

		res = PQexec(conn, "BEGIN");
		if (PQresultStatus(res) != PGRES_COMMAND_OK) {
			e = can_I_retry(res, &fatal_cnt);
			if (e == NULL)
				goto retry;
			PQclear(res);
			goto end;
		}
		PQclear(res);

		GFARM_MALLOC_ARRAY(new_hostname, strlen(hostname) + 1 + 1);
		if (new_hostname == NULL) {
			e = GFARM_ERR_NO_MEMORY;
			goto end;
		}
		sprintf(new_hostname, "#%s", hostname);

		e = hostaliases_remove(hostname);
		if (e != NULL)
			goto end;
		paramValues[0] = hostname;
		paramValues[1] = new_hostname;
		res = PQexecParams(conn,
			"UPDATE Host SET hostname = $2, available = FALSE "
			    "WHERE hostname = $1",
			2, /* number of params */
			NULL, /* param types */
			paramValues,
			NULL, /* param lengths */
			NULL, /* param formats */
			0); /* dummy parameter for result format */
		if (PQresultStatus(res) != PGRES_COMMAND_OK) {
			e = save_pgsql_msg(PQresultErrorMessage(res));
			PQclear(res);
			goto end;
		}
		if (strtol(PQcmdTuples(res), NULL, 0) == 0) {
			e = GFARM_ERR_NO_SUCH_OBJECT;
			PQclear(res);
			goto end;
		}
		PQclear(res);

		if (e == NULL)
			res = PQexec(conn, "COMMIT");
		else
			res = PQexec(conn, "ROLLBACK");
		PQclear(res);
 end:
		if (new_hostname != NULL)
			free(new_hostname);
	}

	return (e);
}

static char *
host_info_get_all(
	char *csql,
	char *isql,
	int nparams,
	const char **paramValues,
	int *np,
	struct gfarm_host_info **infosp)
{
	PGresult *res, *ires, *cres;
	char *e = NULL;
	int i, startrow;
	struct gfarm_host_info *ip;
	int fatal_cnt = 0;

 retry:
	res = PQexec(conn, "BEGIN");
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
		PQclear(res);
		goto end;
	}
	PQclear(res);

	cres = PQexecParams(conn,
		csql,
		nparams,
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		1); /* ask for binary results */
	if (PQresultStatus(cres) != PGRES_TUPLES_OK) {
		e = save_pgsql_msg(PQresultErrorMessage(res));
		PQclear(cres);
		goto clear_cres;
	}
	*np = PQntuples(cres); /* number of hosts */
	if (*np == 0) {
		e = GFARM_ERR_NO_SUCH_OBJECT;
		goto clear_cres;
	}
	GFARM_MALLOC_ARRAY(ip, *np);
	if (ip == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		goto clear_cres;
	}

	ires = PQexecParams(conn,
		isql,
		nparams,
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		1); /* ask for binary results */
	if (PQresultStatus(ires) != PGRES_TUPLES_OK) {
		e = save_pgsql_msg(PQresultErrorMessage(res));
		free(ip);
		goto clear_ires;
	}

	startrow = 0;
	for (i = 0; i < PQntuples(cres); i++) {
		uint64_t nhostaliases = pgsql_get_int64(cres, i, "count");

		gfarm_base_host_info_ops.clear(&ip[i]);
		e = host_info_get_one(ires, startrow, nhostaliases, &ip[i]);
		startrow += (nhostaliases == 0 ? 1 : nhostaliases);
	}
	*infosp = ip;

 clear_ires:
	PQclear(ires);
 clear_cres:
	PQclear(cres);
 end:
	res = PQexec(conn, "END");
	PQclear(res);

	return (e);
}

static char *
gfarm_pgsql_host_info_get_all(
	int *np,
	struct gfarm_host_info **infosp)
{
	char *e;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

	return (host_info_get_all(
		!use_hostid_and_pathid ?
		"SELECT Host.hostname, count(hostalias) "
		    "FROM Host LEFT OUTER JOIN HostAliases "
			"ON Host.hostname = HostAliases.hostname "
		    "GROUP BY Host.hostname "
		    "ORDER BY Host.hostname" :
		"SELECT Host.hostname, count(hostalias) "
		    "FROM Host LEFT OUTER JOIN HostAliases "
			"ON Host.hostname = HostAliases.hostname "
		    "WHERE Host.available "
		    "GROUP BY Host.hostname "
		    "ORDER BY Host.hostname",

		!use_hostid_and_pathid ?
		"SELECT Host.hostname, architecture, ncpu, hostalias "
		    "FROM Host LEFT OUTER JOIN HostAliases "
			"ON Host.hostname = HostAliases.hostname "
		    "ORDER BY Host.hostname, hostalias" :
		"SELECT Host.hostname, architecture, ncpu, hostalias "
		    "FROM Host LEFT OUTER JOIN HostAliases "
			"ON Host.hostname = HostAliases.hostname "
		    "WHERE Host.available "
		    "ORDER BY Host.hostname, hostalias",

		0,
		NULL,
		np,
		infosp));
}

static char *
gfarm_pgsql_host_info_get_by_name_alias(
	const char *name_alias,
	struct gfarm_host_info *info)
{
	const char *paramValues[1];
	char *e;
	int n;
	struct gfarm_host_info *infos;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

	paramValues[0] = name_alias;
	e = host_info_get_all(
		!use_hostid_and_pathid ?
		"SELECT Host.hostname, count(hostalias) "
		    "FROM Host LEFT OUTER JOIN HostAliases "
			"ON Host.hostname = HostAliases.hostname "
		    "WHERE Host.hostname = $1 OR hostalias = $1 "
		    "GROUP BY Host.hostname "
		    "ORDER BY Host.hostname" :
		"SELECT Host.hostname, count(hostalias) "
		    "FROM Host LEFT OUTER JOIN HostAliases "
			"ON Host.hostname = HostAliases.hostname "
		    "WHERE (Host.hostname = $1 OR hostalias = $1) "
			"AND Host.available "
		    "GROUP BY Host.hostname "
		    "ORDER BY Host.hostname",

		!use_hostid_and_pathid ?
		"SELECT Host.hostname, architecture, ncpu, hostalias "
		    "FROM Host LEFT OUTER JOIN HostAliases "
			"ON Host.hostname = HostAliases.hostname "
		    "WHERE Host.hostname = $1 OR hostalias = $1 "
		    "ORDER BY Host.hostname, hostalias" :
		"SELECT Host.hostname, architecture, ncpu, hostalias "
		    "FROM Host LEFT OUTER JOIN HostAliases "
			"ON Host.hostname = HostAliases.hostname "
		    "WHERE (Host.hostname = $1 OR hostalias = $1) "
			"AND Host.available "
		    "ORDER BY Host.hostname, hostalias",

		1,
		paramValues,
		&n,
		&infos);
	if (e != NULL)
		return (e);
	if (n == 0)
		return (GFARM_ERR_UNKNOWN_HOST);
	if (n > 1)
		return (GFARM_ERR_AMBIGUOUS_RESULT);
	*info = infos[0];
	free(infos);
	return (NULL);
}

static char *
gfarm_pgsql_host_info_get_allhost_by_architecture(const char *architecture,
	int *np, struct gfarm_host_info **infosp)
{
	char *e;
	const char *paramValues[1];

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

	paramValues[0] = architecture;
	return (host_info_get_all(
		!use_hostid_and_pathid ?
		"SELECT Host.hostname, count(hostalias) "
		    "FROM Host LEFT OUTER JOIN HostAliases "
			"ON Host.hostname = HostAliases.hostname "
		    "WHERE architecture = $1 "
		    "GROUP BY Host.hostname "
		    "ORDER BY Host.hostname" :
		"SELECT Host.hostname, count(hostalias) "
		    "FROM Host LEFT OUTER JOIN HostAliases "
			"ON Host.hostname = HostAliases.hostname "
		    "WHERE architecture = $1 AND Host.available"
		    "GROUP BY Host.hostname "
		    "ORDER BY Host.hostname",

		!use_hostid_and_pathid ?
		"SELECT Host.hostname, architecture, ncpu, hostalias "
		    "FROM Host LEFT OUTER JOIN HostAliases "
			"ON Host.hostname = HostAliases.hostname "
		    "WHERE architecture = $1 " :
		    "ORDER BY Host.hostname, hostalias"
		"SELECT Host.hostname, architecture, ncpu, hostalias "
		    "FROM Host LEFT OUTER JOIN HostAliases "
			"ON Host.hostname = HostAliases.hostname "
		    "WHERE architecture = $1 AND Host.available"
		    "ORDER BY Host.hostname, hostalias",

		1,
		paramValues,
		np,
		infosp));
}

/**********************************************************************/

static void
path_info_set_field(
	PGresult *res,
	int row,
	struct gfarm_path_info *info)
{
	/* XXX - info->status.st_ino is set not here but at upper level */

	info->pathname = pgsql_get_string(res, row, "pathname");
	info->status.st_mode = pgsql_get_int32(res, row, "mode");
	info->status.st_user = pgsql_get_string(res, row, "username");
	info->status.st_group = pgsql_get_string(res, row, "groupname");
	info->status.st_atimespec.tv_sec =
	    pgsql_get_int64(res, row, "atimesec");
	info->status.st_atimespec.tv_nsec =
	    pgsql_get_int32(res, row, "atimensec");
	info->status.st_mtimespec.tv_sec =
	    pgsql_get_int64(res, row, "mtimesec");
	info->status.st_mtimespec.tv_nsec =
	    pgsql_get_int32(res, row, "mtimensec");
	info->status.st_ctimespec.tv_sec =
	    pgsql_get_int64(res, row, "ctimesec");
	info->status.st_ctimespec.tv_nsec =
	    pgsql_get_int32(res, row, "ctimensec");
	info->status.st_nsections = pgsql_get_int32(res, row, "nsections");
}

static char *
gfarm_pgsql_path_info_get(
	const char *pathname,
	struct gfarm_path_info *info)
{
	const char *paramValues[1];
	PGresult *res;
	char *e;
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

 retry:
	paramValues[0] = pathname;
	res = PQexecParams(conn,
		"SELECT * FROM Path where pathname = $1",
		1, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		1); /* ask for binary results */
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e != NULL) {
			PQclear(res);
			return (e);
		}
		goto retry;
	}
	if (PQntuples(res) == 0) {
		PQclear(res);
		return (GFARM_ERR_NO_SUCH_OBJECT);
	}
	gfarm_base_path_info_ops.clear(info);
	path_info_set_field(res, 0, info);
	PQclear(res);
	return (NULL);
}

static char *
gfarm_pgsql_path_info_set(
	const char *pathname,
	const struct gfarm_path_info *info)
{
	PGresult *res;
	const char *paramValues[11];
	char *e;
	char mode[GFARM_INT32STRLEN + 1];
	char atimesec[GFARM_INT64STRLEN + 1];
	char atimensec[GFARM_INT32STRLEN + 1];
	char mtimesec[GFARM_INT64STRLEN + 1];
	char mtimensec[GFARM_INT32STRLEN + 1];
	char ctimesec[GFARM_INT64STRLEN + 1];
	char ctimensec[GFARM_INT32STRLEN + 1];
	char nsections[GFARM_INT32STRLEN + 1];
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

 retry:
	paramValues[0] = pathname;
	sprintf(mode, "%d", info->status.st_mode);
	paramValues[1] = mode;
	paramValues[2] = info->status.st_user;
	paramValues[3] = info->status.st_group;
	sprintf(atimesec, "%lld", (long long)info->status.st_atimespec.tv_sec);
	paramValues[4] = atimesec;
	sprintf(atimensec, "%d", info->status.st_atimespec.tv_nsec);
	paramValues[5] = atimensec;
	sprintf(mtimesec, "%lld", (long long)info->status.st_mtimespec.tv_sec);
	paramValues[6] = mtimesec;
	sprintf(mtimensec, "%d", info->status.st_mtimespec.tv_nsec);
	paramValues[7] = mtimensec;
	sprintf(ctimesec, "%lld", (long long)info->status.st_ctimespec.tv_sec);
	paramValues[8] = ctimesec;
	sprintf(ctimensec, "%d", info->status.st_ctimespec.tv_nsec);
	paramValues[9] = ctimensec;
	sprintf(nsections, "%d", info->status.st_nsections);
	paramValues[10] = nsections;

	res = PQexecParams(conn,
		"INSERT INTO Path (pathname, mode, username, groupname, "
				   "atimesec, atimensec, "
				   "mtimesec, mtimensec, "
				   "ctimesec, ctimensec, nsections)"
		    "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9 ,$10, $11)",
		11, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0); /* dummy parameter for result format */
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
		if (strcmp(
			   PQresultErrorField(res, PG_DIAG_SQLSTATE),
			   GFARM_PGSQL_ERRCODE_UNIQUE_VIOLATION) == 0) {
			e = GFARM_ERR_ALREADY_EXISTS;
		}
	}
	PQclear(res);
	return (e);
}


static char *
gfarm_pgsql_path_info_replace(
	const char *pathname,
	const struct gfarm_path_info *info)
{
	PGresult *res;
	const char *paramValues[11];
	char *e;
	char mode[GFARM_INT32STRLEN + 1];
	char atimesec[GFARM_INT64STRLEN + 1];
	char atimensec[GFARM_INT32STRLEN + 1];
	char mtimesec[GFARM_INT64STRLEN + 1];
	char mtimensec[GFARM_INT32STRLEN + 1];
	char ctimesec[GFARM_INT64STRLEN + 1];
	char ctimensec[GFARM_INT32STRLEN + 1];
	char nsections[GFARM_INT32STRLEN + 1];
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);
 retry:
	paramValues[0] = pathname;
	sprintf(mode, "%d", info->status.st_mode);
	paramValues[1] = mode;
	paramValues[2] = info->status.st_user;
	paramValues[3] = info->status.st_group;
	sprintf(atimesec, "%lld", (long long)info->status.st_atimespec.tv_sec);
	paramValues[4] = atimesec;
	sprintf(atimensec, "%d", info->status.st_atimespec.tv_nsec);
	paramValues[5] = atimensec;
	sprintf(mtimesec, "%lld", (long long)info->status.st_mtimespec.tv_sec);
	paramValues[6] = mtimesec;
	sprintf(mtimensec, "%d", info->status.st_mtimespec.tv_nsec);
	paramValues[7] = mtimensec;
	sprintf(ctimesec, "%lld", (long long)info->status.st_ctimespec.tv_sec);
	paramValues[8] = ctimesec;
	sprintf(ctimensec, "%d", info->status.st_ctimespec.tv_nsec);
	paramValues[9] = ctimensec;
	sprintf(nsections, "%d", info->status.st_nsections);
	paramValues[10] = nsections;

	res = PQexecParams(conn,
		"UPDATE Path SET mode = $2, username = $3, groupname = $4, "
				"atimesec = $5, atimensec = $6, "
				"mtimesec = $7, mtimensec = $8, "
				"ctimesec = $9, ctimensec = $10, "
				"nsections = $11 "
		    "WHERE pathname = $1",
		11, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0); /* dummy parameter for result format */
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
	} else if (strtol(PQcmdTuples(res), NULL, 0) == 0)
		e = GFARM_ERR_NO_SUCH_OBJECT;
	PQclear(res);
	return (e);
}

static char *
gfarm_pgsql_path_info_remove(const char *pathname)
{
	PGresult *res;
	const char *paramValues[1];
	char *e;
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

 retry:
	paramValues[0] = pathname;
	res = PQexecParams(conn,
		"DELETE FROM Path WHERE pathname = $1",
		1, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0);  /* dummy parameter for result format */
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
	} else if (strtol(PQcmdTuples(res), NULL, 0) == 0)
		e = GFARM_ERR_NO_SUCH_OBJECT;
	PQclear(res);
	return (e);
}

#define COPY_BINARY(data, buf, residual, msg) { \
	if (sizeof(data) > residual) \
		gflog_fatal(msg ": %d bytes needed, but only %d bytes", \
		    (int)sizeof(data), residual); \
	memcpy(&(data), buf, sizeof(data)); \
	buf += sizeof(data); \
	residual -= sizeof(data); \
}

#define COPY_INT32(int32, buf, residual, msg) { \
	assert(sizeof(int32) == sizeof(int32_t)); \
	COPY_BINARY(int32, buf, residual, msg); \
	int32 = ntohl(int32); \
}

static char *
get_value_from_varchar_copy_binary(char **bufp, int *residualp)
{
	int32_t len;
	char *p;

	COPY_INT32(len, *bufp, *residualp, "metdb_pgsql: copy varchar");
	if (len == -1) /* NULL field */
		return (NULL);
	if (len < 0) /* We don't allow that long varchar */
		gflog_fatal("metadb_pgsql: copy varchar length=%d", len);

	if (len > *residualp)
		gflog_fatal("metadb_pgsql: copy varchar %d > %d",
		    len, *residualp);
	GFARM_MALLOC_ARRAY(p, len + 1);
	memcpy(p, *bufp, len);
	p[len] = '\0';
	*bufp += len;
	*residualp -= len;
	return (p);
}

static uint32_t
get_value_from_integer_copy_binary(char **bufp, int *residualp)
{
	int32_t len;
	uint32_t val;

	COPY_INT32(len, *bufp, *residualp, "metadb_pgsql: copy int32 len");
	if (len == -1)
		return (0); /* stopgap for NULL field */
	if (len != sizeof(val))
		gflog_fatal("metadb_pgsql: copy int32 length=%d", len);

	COPY_INT32(val, *bufp, *residualp, "metadb_pgsql: copy int32");
	return (val);
}

static uint64_t
get_value_from_int8_copy_binary(char **bufp, int *residualp)
{
	int32_t len;
	uint64_t val;

	COPY_INT32(len, *bufp, *residualp, "metadb_pgsql: copy int64 len");
	if (len == -1)
		return (0); /* stopgap for NULL field */
	if (len != sizeof(val))
		gflog_fatal("metadb_pgsql: copy int64 length=%d", len);

	COPY_BINARY(val, *bufp, *residualp, "metadb_pgsql: copy int64");
	val = gfarm_ntoh64(val);
	return (val);
}

static void
path_info_set_field_from_copy_binary(
	char *buf, int residual,
	struct gfarm_path_info *info)
{
	uint16_t num_fields;

	/* XXX - info->status.st_ino is set not here but at upper level */

	COPY_BINARY(num_fields, buf, residual, "metadb_pgsql: field number");
	num_fields = ntohs(num_fields);
	if (num_fields < 11) /* allow fields addition in future */
		gflog_fatal("metadb_pgsql: path_info fields = %d", num_fields);

	info->pathname =
	    get_value_from_varchar_copy_binary(&buf, &residual);
	if (use_hostid_and_pathid)
		get_value_from_int8_copy_binary(&buf, &residual);
	info->status.st_mode =
	    get_value_from_integer_copy_binary(&buf, &residual);
	info->status.st_user =
	    get_value_from_varchar_copy_binary(&buf, &residual);
	info->status.st_group =
	    get_value_from_varchar_copy_binary(&buf, &residual);
	info->status.st_atimespec.tv_sec =
	    get_value_from_int8_copy_binary(&buf, &residual);
	info->status.st_atimespec.tv_nsec =
	    get_value_from_integer_copy_binary(&buf, &residual);
	info->status.st_mtimespec.tv_sec =
	    get_value_from_int8_copy_binary(&buf, &residual);
	info->status.st_mtimespec.tv_nsec =
	    get_value_from_integer_copy_binary(&buf, &residual);
	info->status.st_ctimespec.tv_sec =
	    get_value_from_int8_copy_binary(&buf, &residual);
	info->status.st_ctimespec.tv_nsec =
	    get_value_from_integer_copy_binary(&buf, &residual);
	info->status.st_nsections =
	    get_value_from_integer_copy_binary(&buf, &residual);
}

/* XXX - this is for a stopgap implementation of gfs_opendir() */
#define COPY_BINARY_SIGNATURE_LEN		11
#define COPY_BINARY_FLAGS_FIELD_LEN		4
#define COPY_BINARY_HEADER_EXTENSION_AREA_LEN	4
#define COPY_BINARY_HEADER_LEN (COPY_BINARY_SIGNATURE_LEN + \
	    COPY_BINARY_FLAGS_FIELD_LEN +COPY_BINARY_HEADER_EXTENSION_AREA_LEN)
#define COPY_BINARY_TRAILER_LEN			2
#define PQ_GET_COPY_DATA_DONE			-1
#define PQ_GET_COPY_DATA_ERROR			-2

#define	COPY_BINARY_FLAGS_CRITICAL		0xffff0000
#define	COPY_BINARY_TRAILER_VALUE		-1

static char *
gfarm_pgsql_path_info_get_all_foreach(
	void (*callback)(void *, struct gfarm_path_info *),
	void *closure)
{
	PGresult *res;
	char *e, *buf, *bp;
	int ret;
	uint32_t header_flags, extension_area_len;
	int16_t trailer;
	struct gfarm_path_info info;
	int fatal_cnt = 0;

	static const char binary_signature[COPY_BINARY_SIGNATURE_LEN] =
		"PGCOPY\n\377\r\n\0";

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);
 retry:
	res = PQexec(conn,
		"COPY Path to STDOUT BINARY");
	if (PQresultStatus(res) != PGRES_COPY_OUT) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
		PQclear(res);
		return (e);
	}
	PQclear(res);

	ret = PQgetCopyData(conn, &buf,	0);
	if (ret < COPY_BINARY_HEADER_LEN + COPY_BINARY_TRAILER_LEN ||
	    memcmp(buf, binary_signature, COPY_BINARY_SIGNATURE_LEN) != 0) {
		gflog_fatal("gfarm_pgsql_path_info_get_all_foreach: "
		    "Fatal Error, COPY file signature not recognized: %d",ret);
	}
	bp = buf;
	bp  += COPY_BINARY_SIGNATURE_LEN;
	ret -= COPY_BINARY_SIGNATURE_LEN;

	COPY_INT32(header_flags, bp, ret, "metadb_pgsql: COPY header flag");
	if (header_flags & COPY_BINARY_FLAGS_CRITICAL)
		gflog_fatal("gfarm_pgsql_path_info_get_all_foreach: "
		    "Fatal Error, COPY file protocol incompatible: 0x%08x",
		    header_flags);

	COPY_INT32(extension_area_len, bp, ret,
	    "metadb_pgsql: COPY extension area length");
	if (ret < extension_area_len)
		gflog_fatal("gfarm_pgsql_path_info_get_all_foreach: "
		    "Fatal Error, COPY file extension_area too short: %d < %d",
		    ret, extension_area_len);
	bp  += extension_area_len;
	ret -= extension_area_len;

	for (;;) {
		if (ret < COPY_BINARY_TRAILER_LEN)
			gflog_fatal("gfarm_pgsql_path_info_get_all_foreach: "
			    "Fatal error, COPY file trailer too short: %d",
			    ret);
		/* don't use COPY_BINARY() here to not proceed the pointer */
		memcpy(&trailer, bp, sizeof(trailer));
		trailer = ntohs(trailer);

		if (trailer == COPY_BINARY_TRAILER_VALUE) {
			PQfreemem(buf);
			/* make sure that the COPY is done */
			ret = PQgetCopyData(conn, &buf, 0);
			if (ret >= 0)
				gflog_fatal(
				    "gfarm_pgsql_path_info_get_all_foreach: "
				    "Fatal error, COPY file data after trailer"
				    ": %d", ret);
			break;
		}
		gfarm_base_path_info_ops.clear(&info);
		path_info_set_field_from_copy_binary(bp, ret, &info);
		if (gfarm_base_path_info_ops.validate(&info))
			(*callback)(closure, &info);
		gfarm_base_path_info_ops.free(&info);
		PQfreemem(buf);

		ret = PQgetCopyData(conn, &buf, 0);
		bp = buf;
		if (ret < 0) {
			gflog_warning("gfarm_pgsql_path_info_get_all_foreach: "
			    "warning: COPY file expected end of data");
			break;
		}
	}
	if (buf != NULL)
		gflog_warning("gfarm_pgsql_path_info_get_all_foreach: "
		    "warning: COPY file NULL expected, possibly leak?");
	if (ret == PQ_GET_COPY_DATA_ERROR) {
		e = save_pgsql_msg(PQerrorMessage(conn));
		return (e);
	}
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = save_pgsql_msg(PQresultErrorMessage(res));
	}
	PQclear(res);
	return (e);
}

#if 0 /* GFarmFile history isn't actually used yet */

/* get GFarmFiles which were created by the program */
static char *
gfarm_pgsql_file_history_get_allfile_by_program(
	const char *program,
	int *np,
	char ***gfarm_files_p)
{
	char *e;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);
	return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED);
}

/* get GFarmFiles which were created from the file as a input */
static char *
gfarm_pgsql_file_history_get_allfile_by_file(
	const char *input_gfarm_file,
	int *np,
	char ***gfarm_files_p)
{
	char *e;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);
	return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED);
}

#endif /* GFarmFile history isn't actually used yet */

/**********************************************************************/

static void
path_info_xattr_set_field(
	PGresult *res,
	int row,
	struct gfarm_path_info_xattr *info)
{
	info->pathname = pgsql_get_string(res, row, "pathname");
	info->xattr = pgsql_get_string(res, row, "xattribute");
}

static char *
gfarm_pgsql_path_info_xattr_get(
	const char *pathname,
	struct gfarm_path_info_xattr *info)
{
	const char *paramValues[1];
	PGresult *res;
	char *e;
	int fatal_cnt = 0;

	if (!use_hostid_and_pathid)
		return (GFARM_ERR_OPERATION_NOT_SUPPORTED);

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

 retry:
	paramValues[0] = pathname;
	res = PQexecParams(conn,
		"SELECT pathname, PathXattr.* "
		    "FROM Path, PathXAttr WHERE "
		    "pathname = $1 AND PathXAttr.pathid = Path.pathid",
		1, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		1); /* ask for binary results */
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e != NULL) {
			PQclear(res);
			return (e);
		}
		goto retry;
	}
	if (PQntuples(res) == 0) {
		PQclear(res);
		return (GFARM_ERR_NO_SUCH_OBJECT);
	}
	gfarm_base_path_info_xattr_ops.clear(info);
	path_info_xattr_set_field(res, 0, info);
	PQclear(res);
	return (NULL);
}

static char *
gfarm_pgsql_path_info_xattr_set(
	const struct gfarm_path_info_xattr *info)
{
	PGresult *res;
	const char *paramValues[2];
	char *e;
	int fatal_cnt = 0;

	if (!use_hostid_and_pathid)
		return (GFARM_ERR_OPERATION_NOT_SUPPORTED);

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

 retry:
	paramValues[0] = info->pathname;
	paramValues[1] = info->xattr;

	res = PQexecParams(conn,
		"INSERT INTO PathXAttr (pathid, xattribute) VALUES ("
			"(SELECT pathid FROM Path WHERE pathname = $1), $2)",
		2, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0); /* dummy parameter for result format */
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
		if (strcmp(
			   PQresultErrorField(res, PG_DIAG_SQLSTATE),
			   GFARM_PGSQL_ERRCODE_UNIQUE_VIOLATION) == 0) {
			e = GFARM_ERR_ALREADY_EXISTS;
		}
	}
	PQclear(res);
	return (e);
}

static char *
gfarm_pgsql_path_info_xattr_replace(
	const struct gfarm_path_info_xattr *info)
{
	PGresult *res;
	const char *paramValues[2];
	char *e;
	int fatal_cnt = 0;

	if (!use_hostid_and_pathid)
		return (GFARM_ERR_OPERATION_NOT_SUPPORTED);

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

 retry:
	paramValues[0] = info->pathname;
	paramValues[1] = info->xattr;

	res = PQexecParams(conn,
		"UPDATE PathXAttr SET xattribute = $2 "
		    "WHERE "
			"pathid=(SELECT pathid from Path WHERE pathname = $1) ",
		2, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0); /* dummy parameter for result format */
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
	} else if (strtol(PQcmdTuples(res), NULL, 0) == 0)
		e = GFARM_ERR_NO_SUCH_OBJECT;
	PQclear(res);
	return (e);
}

static char *
gfarm_pgsql_path_info_xattr_remove(const char *pathname)
{
	PGresult *res;
	const char *paramValues[1];
	char *e;
	int fatal_cnt = 0;

	if (!use_hostid_and_pathid)
		return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED);

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

 retry:
	paramValues[0] = pathname;
	res = PQexecParams(conn,
		"DELETE FROM PathXAttr "
		    "WHERE "
			"pathid=(SELECT pathid from Path WHERE pathname = $1) ",
		1, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0);  /* dummy parameter for result format */
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
	} else if (strtol(PQcmdTuples(res), NULL, 0) == 0)
		e = GFARM_ERR_NO_SUCH_OBJECT;
	PQclear(res);
	return (e);
}

/**********************************************************************/

static void
file_section_info_set_field(
	PGresult *res,
	int row,
	struct gfarm_file_section_info *info)
{
	info->pathname = pgsql_get_string(res, row, "pathname");
	info->section = pgsql_get_string(res, row, "section");
	info->filesize= pgsql_get_int64(res, row, "filesize");
	info->checksum_type = pgsql_get_string(res, row, "checksumType");
	info->checksum = pgsql_get_string(res, row, "checksum");
}

static char *
gfarm_pgsql_file_section_info_get(
	const char *pathname,
	const char *section,
	struct gfarm_file_section_info *info)
{
	const char *paramValues[2];
	PGresult *res;
	char *e;
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);
 retry:
	paramValues[0] = pathname;
	paramValues[1] = section;
	res = PQexecParams(conn,
		!use_hostid_and_pathid ?
		"SELECT * FROM FileSection WHERE pathname = $1 "
		    "AND section = $2" :
		"SELECT pathname, FileSection.* "
		    "FROM Path, FileSection WHERE "
			"pathname = $1 "
			"AND FileSection.pathid = Path.pathid "
			"AND section = $2",
		2, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		1); /* ask for binary results */
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
		PQclear(res);
		return (e);
	}
	if (PQntuples(res) == 0) {
		PQclear(res);
		return (GFARM_ERR_NO_SUCH_OBJECT);
	}
	gfarm_base_file_section_info_ops.clear(info);
	file_section_info_set_field(res, 0, info);
	PQclear(res);
	return (NULL);
}

static char *
gfarm_pgsql_file_section_info_set(
	const char *pathname,
	const char *section,
	const struct gfarm_file_section_info *info)
{
	PGresult *res;
	const char *paramValues[5];
	char *e;
	char filesize[GFARM_INT64STRLEN + 1];
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

 retry:
	paramValues[0] = pathname;
	paramValues[1] = section;
	sprintf(filesize, "%lld", (long long)info->filesize);
	paramValues[2] = filesize;
	paramValues[3] = info->checksum_type;
	paramValues[4] = info->checksum;
	res = PQexecParams(conn,
		!use_hostid_and_pathid ?
		"INSERT INTO FileSection (pathname, section, filesize,"
			"checksumType, checksum) "
		     "VALUES ($1, $2, $3, $4, $5)" :
		"INSERT INTO FileSection (pathid, section, filesize, "
			"checksumType, checksum) VALUES ("
			"(SELECT pathid FROM Path WHERE pathname = $1),"
			"$2, $3, $4, $5)",
		5, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0); /* dummy parameter for result format */
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
		if (strcmp(
			   PQresultErrorField(res, PG_DIAG_SQLSTATE),
			   GFARM_PGSQL_ERRCODE_UNIQUE_VIOLATION) == 0) {
			e = GFARM_ERR_ALREADY_EXISTS;
		}
	}
	PQclear(res);
	return (e);
}

static char *
gfarm_pgsql_file_section_info_replace(
	const char *pathname,
	const char *section,
	const struct gfarm_file_section_info *info)
{
	PGresult *res;
	const char *paramValues[5];
	char *e;
	char filesize[GFARM_INT64STRLEN + 1];
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);
 retry:
	paramValues[0] = pathname;
	paramValues[1] = section;
	sprintf(filesize, "%lld", (long long)info->filesize);
	paramValues[2] = filesize;
	paramValues[3] = info->checksum_type;
	paramValues[4] = info->checksum;
	res = PQexecParams(conn,
		!use_hostid_and_pathid ?
		"UPDATE FileSection SET filesize = $3, "
				"checksumType = $4, checksum = $5 "
		    "WHERE pathname = $1 AND section = $2" :
		"UPDATE FileSection SET filesize = $3, "
				"checksumType = $4, checksum = $5 "
		    "WHERE "
			"pathid=(SELECT pathid from Path WHERE pathname = $1) "
			"AND section = $2",
		5, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0); /* dummy parameter for result format */
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
	} else if (strtol(PQcmdTuples(res), NULL, 0) == 0)
		e = GFARM_ERR_NO_SUCH_OBJECT;
	PQclear(res);
	return (e);
}

static char *
gfarm_pgsql_file_section_info_remove(
	const char *pathname,
	const char *section)
{
	PGresult *res;
	const char *paramValues[2];
	char *e;
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);
 retry:
	paramValues[0] = pathname;
	paramValues[1] = section;
	res = PQexecParams(conn,
		!use_hostid_and_pathid ?
		"DELETE FROM FileSection WHERE pathname = $1 AND section = $2":
		"DELETE FROM FileSection WHERE "
			"pathid=(SELECT pathid FROM Path WHERE pathname = $1) "
			"AND section = $2",
		2, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0);  /* dummy parameter for result format */
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
	} else if (strtol(PQcmdTuples(res), NULL, 0) == 0)
		e = GFARM_ERR_NO_SUCH_OBJECT;
	PQclear(res);
	return (e);
}

static char *
gfarm_pgsql_file_section_info_get_all_by_file(
	const char *pathname,
	int *np,
	struct gfarm_file_section_info **infosp)
{
	const char *paramValues[1];
	PGresult *res;
	char *e;
	struct gfarm_file_section_info *ip;
	int i;
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);
 retry:
	paramValues[0] = pathname;
	res = PQexecParams(conn,
		!use_hostid_and_pathid ?
		"SELECT * FROM FileSection WHERE pathname = $1" :
		"SELECT Path.pathname, FileSection.* FROM Path, FileSection "
		    "WHERE pathname = $1 AND FileSection.pathid = Path.pathid",
		1, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		1); /* ask for binary results */
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
		PQclear(res);
		return (e);
	}
	*np = PQntuples(res);
	if (*np == 0) {
		PQclear(res);
		return (GFARM_ERR_NO_SUCH_OBJECT);
	}
	GFARM_MALLOC_ARRAY(ip, *np);
	if (ip == NULL) {
		PQclear(res);
		return (GFARM_ERR_NO_MEMORY);
	}
	for (i = 0; i < *np; i++) {
		gfarm_base_file_section_info_ops.clear(&ip[i]);
		file_section_info_set_field(res, i, &ip[i]);
	}
	*infosp = ip;
	PQclear(res);
	return (NULL);
}

/**********************************************************************/

static void
file_section_copy_info_set_field(
	PGresult *res,
	int row,
	struct gfarm_file_section_copy_info *info)
{
	info->pathname = pgsql_get_string(res, row, "pathname");
	info->section = pgsql_get_string(res, row, "section");
	info->hostname = pgsql_get_string(res, row, "hostname");
}

static char *
gfarm_pgsql_file_section_copy_info_get(
	const char *pathname,
	const char *section,
	const char *hostname,
	struct gfarm_file_section_copy_info *info)
{
	const char *paramValues[3];
	PGresult *res;
	char *e;
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);
 retry:
	paramValues[0] = pathname;
	paramValues[1] = section;
	paramValues[2] = hostname;
	res = PQexecParams(conn,
		!use_hostid_and_pathid ?
		"SELECT * FROM FileSectionCopy where pathname = $1 "
		    "AND section = $2 AND hostname = $3" :
		"SELECT pathname, section, hostname "
		    "FROM Path, FileSectionCopy, Host "
		    "WHERE pathname = $1 AND hostname = $3 "
			"AND FileSectionCopy.pathid = Path.pathid "
			"AND FileSectionCopy.hostid = Host.hostid "
			"AND section = $2",
		3, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		1); /* ask for binary results */
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
		PQclear(res);
		return (e);
	}
	if (PQntuples(res) == 0) {
		PQclear(res);
		return (GFARM_ERR_NO_SUCH_OBJECT);
	}
	gfarm_base_file_section_copy_info_ops.clear(info);
	file_section_copy_info_set_field(res, 0, info);
	PQclear(res);
	return (NULL);
}

static PGresult *
file_section_copy_info_set(
	const char *pathname, const char *section, const char *hostname)
{
	const char *paramValues[3];

	paramValues[0] = pathname;
	paramValues[1] = section;
	paramValues[2] = hostname;
	return (PQexecParams(conn,
		!use_hostid_and_pathid ?
		"INSERT INTO FileSectionCopy (pathname, section, hostname) "
		     "VALUES ($1, $2, $3)" :
		"INSERT INTO FileSectionCopy (pathid, section, hostid) "
		     "VALUES ("
			"(SELECT pathid FROM Path WHERE pathname = $1), "
			"$2, "
			"(SELECT hostid FROM Host WHERE hostname = $3))",
		3, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0) /* dummy parameter for result format */
	    );
}

/* this function is only called when `use_hostid_and_pathid' */
static PGresult *
file_section_copy_info_set_for_removed_host(
	const char *pathname, const char *section, const char *hostname)
{
	PGresult *res = NULL;
	char *h = NULL;
	const char *n;

	if (hostname[0] == '#') {
		n = hostname;
	} else {
		GFARM_MALLOC_ARRAY(h, strlen(hostname) + 1 + 1);
		if (h == NULL)
			return (NULL); /* not enough memory, so... */
		sprintf(h, "#%s", hostname);

		res = file_section_copy_info_set(pathname, section, h);
		if (PQresultStatus(res) == PGRES_COMMAND_OK ||
		    strcmp(PQresultErrorField(res, PG_DIAG_SQLSTATE),
		    GFARM_PGSQL_ERRCODE_NOT_NULL_VIOLATION) != 0) {
			free(h);
			return (res);
		}
		PQclear(res);
		res = NULL;
		n = h;
	}

	if (removed_host_info_set(n))
		res = file_section_copy_info_set(pathname, section, n);

	if (h != NULL)
		free(h);
	return (res);
}

static char *
gfarm_pgsql_file_section_copy_info_set(
	const char *pathname,
	const char *section,
	const char *hostname,
	const struct gfarm_file_section_copy_info *info)
{
	PGresult *res;
	char *e;
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

	/*
	 * If this function is called from "gfdump -r", the hostname
	 * may refer already removed one.  And if the metadata was dump
	 * from v1 schema, it may have "#" at the top of the hostname.
	 */
	if (!use_hostid_and_pathid && hostname[0] == '#')
		hostname++;

 retry:
	res = file_section_copy_info_set(pathname, section, hostname);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;

		/*
		 * deal with the case that this function is called from
		 * "gfrump -r", and this host was removed in the metadata.
		 */
		if (use_hostid_and_pathid &&
		    strcmp(PQresultErrorField(res, PG_DIAG_SQLSTATE),
		    GFARM_PGSQL_ERRCODE_NOT_NULL_VIOLATION) == 0) {
			PGresult *res2 ;

			/* the column that violated not-null may be "hostid" */
			res2 = file_section_copy_info_set_for_removed_host(
			    pathname, section, hostname);
			if (res2 != NULL) {
				PQclear(res);
				res = res2;
				if (PQresultStatus(res) == PGRES_COMMAND_OK)
					e = NULL;
			}
		}

		if (e != NULL &&
		    strcmp(PQresultErrorField(res, PG_DIAG_SQLSTATE),
			   GFARM_PGSQL_ERRCODE_UNIQUE_VIOLATION) == 0) {
			e = GFARM_ERR_ALREADY_EXISTS;
		}
	}
	PQclear(res);
	return (e);
}

static char *
gfarm_pgsql_file_section_copy_info_remove(
	const char *pathname,
	const char *section,
	const char *hostname)
{
	PGresult *res;
	const char *paramValues[3];
	char *e;
	int fatal_cnt = 0;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);
 retry:
	paramValues[0] = pathname;
	paramValues[1] = section;
	paramValues[2] = hostname;
	res = PQexecParams(conn,
		!use_hostid_and_pathid ?
		"DELETE FROM FileSectionCopy "
		    "WHERE pathname = $1 AND section = $2 AND hostname = $3" :
		"DELETE FROM FileSectionCopy WHERE "
			"pathid=(SELECT pathid FROM Path WHERE pathname = $1) "
			"AND section = $2 AND "
			"hostid=(SELECT hostid FROM Host WHERE hostname = $3)",
		3, /* number of params */
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		0);  /* dummy parameter for result format */
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
	} else if (strtol(PQcmdTuples(res), NULL, 0) == 0)
		e = GFARM_ERR_NO_SUCH_OBJECT;
	PQclear(res);
	return (e);
}

static char *
file_section_copy_info_get_all(
	const char *sql,
	int nparams,
	const char **paramValues,
	int *np,
	struct gfarm_file_section_copy_info **infosp)
{
	PGresult *res;
	char *e;
	struct gfarm_file_section_copy_info *ip;
	int i;
	int fatal_cnt = 0;

 retry:
	res = PQexecParams(conn,
		sql,
		nparams,
		NULL, /* param types */
		paramValues,
		NULL, /* param lengths */
		NULL, /* param formats */
		1); /* ask for binary results */
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		e = can_I_retry(res, &fatal_cnt);
		if (e == NULL)
			goto retry;
		PQclear(res);
		return (e);
	}
	*np = PQntuples(res);
	if (*np == 0) {
		PQclear(res);
		return (GFARM_ERR_NO_SUCH_OBJECT);
	}
	GFARM_MALLOC_ARRAY(ip , *np);
	if (ip == NULL) {
		PQclear(res);
		return (GFARM_ERR_NO_MEMORY);
	}
	for (i = 0; i < *np; i++) {
		gfarm_base_file_section_copy_info_ops.clear(&ip[i]);
		file_section_copy_info_set_field(res, i, &ip[i]);
	}
	*infosp = ip;
	PQclear(res);
	return (NULL);
}

static char *
gfarm_pgsql_file_section_copy_info_get_all_by_file(
	const char *pathname,
	int *np,
	struct gfarm_file_section_copy_info **infosp)
{
	char *e;
	const char *paramValues[1];

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

	paramValues[0] = pathname;
	return (file_section_copy_info_get_all(
		!use_hostid_and_pathid ?
		"SELECT * FROM FileSectionCopy where pathname = $1" :
		"SELECT pathname, section, hostname "
		    "FROM Path, FileSectionCopy, Host WHERE "
			"pathname = $1 "
			"AND FileSectionCopy.pathid = Path.pathid "
			"AND Host.hostid = FileSectionCopy.hostid",
		1,
		paramValues,
		np,
		infosp));
}

static char *
gfarm_pgsql_file_section_copy_info_get_all_by_section(
	const char *pathname,
	const char *section,
	int *np,
	struct gfarm_file_section_copy_info **infosp)
{
	const char *paramValues[2];
	char *e;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

	paramValues[0] = pathname;
	paramValues[1] = section;
	return (file_section_copy_info_get_all(
		!use_hostid_and_pathid ?
		"SELECT * FROM FileSectionCopy "
		    "WHERE pathname = $1 AND section = $2" :
		"SELECT pathname, section, hostname "
		    "FROM Path, FileSectionCopy, Host WHERE "
			"pathname = $1 "
			"AND FileSectionCopy.pathid = Path.pathid "
			"AND section = $2 "
			"AND Host.hostid = FileSectionCopy.hostid",
		2,
		paramValues,
		np,
		infosp));
}

static char *
gfarm_pgsql_file_section_copy_info_get_all_by_host(
	const char *hostname,
	int *np,
	struct gfarm_file_section_copy_info **infosp)
{
	const char *paramValues[1];
	char *e;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);

	paramValues[0] = hostname;
	return (file_section_copy_info_get_all(
		!use_hostid_and_pathid ?
		"SELECT * FROM FileSectionCopy where hostname = $1" :
		"SELECT pathname, section, hostname "
		    "FROM Path, FileSectionCopy, Host WHERE "
			"hostname = $1 "
			"AND FileSectionCopy.hostid = Host.hostid "
			"AND Path.pathid = FileSectionCopy.pathid",
		1,
		paramValues,
		np,
		infosp));
}

/**********************************************************************/

#if 0 /* GFarmFile history isn't actually used yet */

static char *
gfarm_pgsql_file_history_get(
	const char *gfarm_file,
	struct gfarm_file_history *info)
{
	char *e;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);
	return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED);
}

static char *
gfarm_pgsql_file_history_set(
	const char *gfarm_file,
	const struct gfarm_file_history *info)
{
	char *e;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);
	return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED);
}

static char *
gfarm_pgsql_file_history_remove(const char *gfarm_file)
{
	char *e;

	if ((e = gfarm_pgsql_check()) != NULL)
		return (e);
	return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED);
}

#endif /* GFarmFile history isn't actually used yet */

/**********************************************************************/

const struct gfarm_metadb_internal_ops gfarm_pgsql_metadb_ops = {
	gfarm_pgsql_initialize,
	gfarm_pgsql_terminate,

	gfarm_pgsql_host_info_get,
	gfarm_pgsql_host_info_remove_hostaliases,
	gfarm_pgsql_host_info_set,
	gfarm_pgsql_host_info_replace,
	gfarm_pgsql_host_info_remove,
	gfarm_pgsql_host_info_get_all,
	gfarm_pgsql_host_info_get_by_name_alias,
	gfarm_pgsql_host_info_get_allhost_by_architecture,

	gfarm_pgsql_path_info_get,
	gfarm_pgsql_path_info_set,
	gfarm_pgsql_path_info_replace,
	gfarm_pgsql_path_info_remove,
	gfarm_pgsql_path_info_get_all_foreach,

	gfarm_pgsql_file_section_info_get,
	gfarm_pgsql_file_section_info_set,
	gfarm_pgsql_file_section_info_replace,
	gfarm_pgsql_file_section_info_remove,
	gfarm_pgsql_file_section_info_get_all_by_file,

	gfarm_pgsql_file_section_copy_info_get,
	gfarm_pgsql_file_section_copy_info_set,
	gfarm_pgsql_file_section_copy_info_remove,
	gfarm_pgsql_file_section_copy_info_get_all_by_file,
	gfarm_pgsql_file_section_copy_info_get_all_by_section,
	gfarm_pgsql_file_section_copy_info_get_all_by_host,

	gfarm_pgsql_path_info_xattr_get,
	gfarm_pgsql_path_info_xattr_set,
	gfarm_pgsql_path_info_xattr_replace,
	gfarm_pgsql_path_info_xattr_remove,
};
