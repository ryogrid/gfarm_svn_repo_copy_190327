/*
 * $Id: gfs_client_dir.c 3025 2006-07-28 07:48:38Z soda $
 *
 * This module depends on both gfs_client.c and url.c and also config.c
 */

#include <sys/types.h>
#include <sys/socket.h> /* socklen_t for "gfs_client.h" */
#include <stdlib.h>
#include <string.h>

#include <gfarm/gfarm.h>

#include "config.h"
#include "gfs_client.h"
#include "gfs_misc.h"

static char *
gfs_client_mkdir_p(
	struct gfs_connection *gfs_server, char *canonic_dir)
{
	struct gfs_stat stata;
	gfarm_mode_t mode;
	char *e, *user;

	/*
	 * gfarm_path_dir(3) may return '.'.
	 * This means the spool root directory.
	 */
	if (strcmp(canonic_dir, "/") == 0 || strcmp(canonic_dir, ".") == 0)
		return (NULL); /* should exist */

	user = gfarm_get_global_username();
	if (user == NULL)
		return ("gfs_client_mkdir_p(): programming error, "
			"gfarm library isn't properly initialized");

	e = gfs_stat_canonical_path(canonic_dir, &stata);
	if (e != NULL)
		return (e);
	mode = stata.st_mode;
	/*
	 * XXX - if the owner of a directory is not the same, create a
	 * directory with permission 0777 - This should be fixed in
	 * the next major release.
	 */
	if (strcmp(stata.st_user, user) != 0)
		mode |= 0777;
	gfs_stat_free(&stata);
	if (!GFARM_S_ISDIR(mode))
		return (GFARM_ERR_NOT_A_DIRECTORY);

	if (gfs_client_exist(gfs_server, canonic_dir)
	    == GFARM_ERR_NO_SUCH_OBJECT) {
		char *par_dir;

		par_dir = gfarm_path_dir(canonic_dir);
		if (par_dir == NULL)
			return (GFARM_ERR_NO_MEMORY);
		e = gfs_client_mkdir_p(gfs_server, par_dir);
		free(par_dir);
		if (e != NULL)
			return (e);
		
		e = gfs_client_mkdir(gfs_server, canonic_dir, mode);
	}
	return (e);
}

char *
gfs_client_mk_parent_dir(struct gfs_connection *gfs_server, char *canonic_dir)
{
	char *par_dir, *e;

	par_dir = gfarm_path_dir(canonic_dir);
	if (par_dir == NULL)
		return (GFARM_ERR_NO_MEMORY);

	if (gfs_client_exist(gfs_server, par_dir) == GFARM_ERR_NO_SUCH_OBJECT)
		e = gfs_client_mkdir_p(gfs_server, par_dir);
	else
		e = GFARM_ERR_ALREADY_EXISTS;

	free(par_dir);

	return (e);
}

char *
gfs_client_link_faulttolerant(const char *hostname, char *from, char *to,
	struct gfs_connection **gfs_serverp, char **op_errorp)
{
	char *e, *e2;
	struct gfs_connection *gfs_server;

	e = gfs_client_link_with_reconnection(hostname, from, to,
	    &gfs_server, &e2);
	if (e != NULL)
		return (e);

	e = e2;
	if (e2 != NULL) {
		if (e == GFARM_ERR_ALREADY_EXISTS)
			e2 = gfs_client_unlink(gfs_server, to);
		else if (e == GFARM_ERR_NO_SUCH_OBJECT)
			e2 = gfs_client_mk_parent_dir(gfs_server, to);
		if (e2 == NULL)
			e = gfs_client_link(gfs_server, from, to);
	}

	if (gfs_serverp != NULL)
		*gfs_serverp = gfs_server;
	else
		gfs_client_connection_free(gfs_server);

	if (op_errorp != NULL)
		*op_errorp = e;

	return (NULL);
}
