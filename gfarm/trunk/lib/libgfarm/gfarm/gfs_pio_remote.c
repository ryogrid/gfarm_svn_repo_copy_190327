/*
 * pio operations for remote fragment
 *
 * $Id: gfs_pio_remote.c 2916 2006-06-30 11:42:44Z soda $
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <libgen.h>
#include <sys/socket.h> /* struct sockaddr */
#include <openssl/evp.h>
#include <gfarm/gfarm.h>
#include "host.h"
#include "config.h"
#include "gfs_proto.h"
#include "gfs_client.h"
#include "gfs_pio.h"
#include "gfs_misc.h"

static char *
gfs_pio_remote_storage_close(GFS_File gf)
{
	char *e, *e_save;
	struct gfs_file_section_context *vc = gf->view_context;
	struct gfs_connection *gfs_server = vc->storage_context;

	/*
	 * Do not close remote file from a child process because its
	 * open file count is not incremented.
	 * XXX - This behavior is not the same as expected, but better
	 * than closing the remote file.
	 */
	if (vc->pid != getpid())
		return (NULL);

	e_save = gfs_client_close(gfs_server, vc->fd);
	e = gfs_client_connection_free(gfs_server);
	return (e_save != NULL ? e_save : e);
}

static char *
gfs_pio_remote_storage_write(GFS_File gf, const char *buffer, size_t size,
			    size_t *lengthp)
{
	struct gfs_file_section_context *vc = gf->view_context;
	struct gfs_connection *gfs_server = vc->storage_context;

	/*
	 * buffer beyond GFS_PROTO_MAX_IOSIZE are just ignored by gfsd,
	 * we don't perform such GFS_PROTO_WRITE request, because it's
	 * inefficient.
	 * Note that upper gfs_pio layer should care this partial write.
	 */
	if (size > GFS_PROTO_MAX_IOSIZE)
		size = GFS_PROTO_MAX_IOSIZE;
	return (gfs_client_write(gfs_server, vc->fd, buffer, size, lengthp));
}

static char *
gfs_pio_remote_storage_read(GFS_File gf, char *buffer, size_t size,
			   size_t *lengthp)
{
	struct gfs_file_section_context *vc = gf->view_context;
	struct gfs_connection *gfs_server = vc->storage_context;

	/*
	 * Unlike gfs_pio_remote_storage_write(), we don't care
	 * buffer size here, because automatic i/o size truncation
	 * performed by gfsd isn't inefficient for read case.
	 * Note that upper gfs_pio layer should care the partial read.
	 */
	return (gfs_client_read(gfs_server, vc->fd, buffer, size, lengthp));
}

static char *
gfs_pio_remote_storage_seek(GFS_File gf, file_offset_t offset, int whence,
			   file_offset_t *resultp)
{
	struct gfs_file_section_context *vc = gf->view_context;
	struct gfs_connection *gfs_server = vc->storage_context;

	return (gfs_client_seek(gfs_server, vc->fd, offset, whence, resultp));
}

static char *
gfs_pio_remote_storage_ftruncate(GFS_File gf, file_offset_t length)
{
	struct gfs_file_section_context *vc = gf->view_context;
	struct gfs_connection *gfs_server = vc->storage_context;

	return (gfs_client_ftruncate(gfs_server, vc->fd, length));
}

static char *
gfs_pio_remote_storage_fsync(GFS_File gf, int operation)
{
	struct gfs_file_section_context *vc = gf->view_context;
	struct gfs_connection *gfs_server = vc->storage_context;

	return (gfs_client_fsync(gfs_server, vc->fd, operation));
}

static char *
gfs_pio_remote_storage_fstat(GFS_File gf, struct stat *st)
{
	struct gfs_file_section_context *vc = gf->view_context;
	struct gfs_connection *gfs_server = vc->storage_context;

	return (gfs_client_fstat(gfs_server, vc->fd, st));
}

static char *
gfs_pio_remote_storage_calculate_digest(GFS_File gf, char *digest_type,
				       size_t digest_size,
				       size_t *digest_lengthp,
				       unsigned char *digest,
				       file_offset_t *filesizep)
{
	struct gfs_file_section_context *vc = gf->view_context;
	struct gfs_connection *gfs_server = vc->storage_context;

	return (gfs_client_digest(gfs_server, vc->fd, digest_type, digest_size,
				  digest_lengthp, digest, filesizep));
}

static int
gfs_pio_remote_storage_fd(GFS_File gf)
{
	struct gfs_file_section_context *vc = gf->view_context;
	struct gfs_connection *gfs_server = vc->storage_context;

	return (gfs_client_connection_fd(gfs_server));
}

struct gfs_storage_ops gfs_pio_remote_storage_ops = {
	gfs_pio_remote_storage_close,
	gfs_pio_remote_storage_write,
	gfs_pio_remote_storage_read,
	gfs_pio_remote_storage_seek,
	gfs_pio_remote_storage_ftruncate,
	gfs_pio_remote_storage_fsync,
	gfs_pio_remote_storage_fstat,
	gfs_pio_remote_storage_calculate_digest,
	gfs_pio_remote_storage_fd,
};

char *
gfs_pio_open_remote_section(GFS_File gf, char *hostname, int flags)
{
	struct gfs_file_section_context *vc = gf->view_context;
	char *e, *e2, *path_section;
	struct gfs_connection *gfs_server;
	/*
	 * We won't use GFARM_FILE_EXCLUSIVE flag for the actual storage
	 * level access (at least for now) to avoid the effect of
	 * remaining junk files.
	 * It's already handled anyway at the metadata level.
	 *
	 * NOTE: Same thing must be done in gfs_pio_local.c.
	 */
	int oflags = (gf->open_flags & ~GFARM_FILE_EXCLUSIVE) |
	    (flags & GFARM_FILE_CREATE);
	int fd;
	struct sockaddr peer_addr;

	e = gfarm_host_address_get(hostname, gfarm_spool_server_port,
	    &peer_addr, NULL);
	if (e != NULL)
		return (e);

	e = gfarm_path_section(gf->pi.pathname, vc->section, &path_section);
	if (e != NULL)
		return (e);

	e = gfs_client_open_with_reconnect_addr(
	    vc->canonical_hostname, &peer_addr, path_section, oflags,
	    gf->pi.status.st_mode & GFARM_S_ALLPERM,
	    &gfs_server, &e2, &fd);
	if (e != NULL) {
		free(path_section);
		return (e);
	}
	vc->storage_context = gfs_server;
		
	/* FT - the parent directory may be missing */
	if (e2 == GFARM_ERR_NO_SUCH_OBJECT
	    && (oflags & GFARM_FILE_CREATE) != 0) {
		/* the parent directory can be created by some other process */
		(void)gfs_client_mk_parent_dir(
			gfs_server, gf->pi.pathname);
		e2 = gfs_client_open(gfs_server, path_section, oflags,
			gf->pi.status.st_mode & GFARM_S_ALLPERM, &fd);
	}
	/* FT - physical file should be missing */
	if (e2 == GFARM_ERR_NO_SUCH_OBJECT
	    && (oflags & GFARM_FILE_CREATE) == 0) {
		/* Delete the section copy info */
		/* section copy may be removed by some other process */
		(void)gfarm_file_section_copy_info_remove(gf->pi.pathname,
			vc->section, vc->canonical_hostname);
		e2 = GFARM_ERR_INCONSISTENT_RECOVERABLE;
	}

	free(path_section);
	if (e2 != NULL) {
		gfs_client_connection_free(gfs_server);
		return (e2);
	}

	vc->ops = &gfs_pio_remote_storage_ops;
	vc->fd = fd;
	vc->pid = getpid();
	return (NULL);
}
