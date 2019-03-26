/*
 * $Id: gfs_client.c 3689 2007-04-03 17:41:40Z n-soda $
 */

#define _POSIX_PII_SOCKET /* to use struct msghdr on Tru64 */
#include <assert.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h> /* sprintf() */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <openssl/evp.h>

#include <gfarm/gfarm_config.h>
#include <gfarm/gfarm_error.h>
#include <gfarm/gfarm_misc.h>
#include <gfarm/gfs.h>
#include <gfarm/gfarm_metadb.h>

#if defined(SCM_RIGHTS) && \
		(!defined(sun) || (!defined(__svr4__) && !defined(__SVR4)))
#define HAVE_MSG_CONTROL 1
#endif

#include "gfutil.h"
#include "gfevent.h"
#include "hash.h"

#include "iobuffer.h"
#include "xxx_proto.h"
#include "io_fd.h"
#include "host.h"
#include "param.h"
#include "sockopt.h"
#include "auth.h"
#include "config.h"
#include "conn_hash.h"
#include "gfs_proto.h"
#include "gfs_client.h"

#define GFS_CLIENT_CONNECT_TIMEOUT	30 /* seconds */
#define GFS_CLIENT_COMMAND_TIMEOUT	20 /* seconds */

#define XAUTH_NEXTRACT_MAXLEN	512

static char GFARM_ERR_GFSD_ABORTED[] = "gfsd aborted";

struct gfs_connection {
	struct gfs_connection *next, *prev; /* doubly linked circular list */
	struct gfarm_hash_entry *hash_entry;

	struct xxx_connection *conn;
	char *hostname;
	enum gfarm_auth_method auth_method;

	/* reference counters */
	int acquired;
	int opened;

	void *context; /* work area for RPC (esp. GFS_PROTO_COMMAND) */
};

/*
 * return TRUE,  if created by gfs_client_connection_acquire() && still cached.
 * return FALSE, if created by gfs_client_connect() || purged from cache.
 */
#define gfs_client_connection_is_cached(gfs_server) \
	((gfs_server)->hash_entry != NULL)

/* doubly linked circular list head to see LRU connection */
static struct gfs_connection connection_list_head = {
	&connection_list_head, &connection_list_head
};

static int free_connections = 0;

#define SERVER_HASHTAB_SIZE	3079	/* prime number */

static struct gfarm_hash_table *gfs_server_hashtab = NULL;
static int gfsd_version_1_2_or_earlier;

static char *(*gfs_client_hook_for_connection_error)(const char *) = NULL;

/*
 * Currently this supports only one hook function.
 * And the function is always gfarm_schedule_host_cache_clear_auth() (or NULL).
 */
void
gfs_client_add_hook_for_connection_error(char *(*hook)(const char *))
{
	gfs_client_hook_for_connection_error = hook;
}

int
gfs_client_is_connection_error(const char *e)
{
	return (
	    e == GFARM_ERR_BROKEN_PIPE ||
	    e == GFARM_ERR_PROTOCOL ||
	    e == GFARM_ERR_NETWORK_IS_DOWN ||
	    e == GFARM_ERR_NETWORK_IS_UNREACHABLE ||
	    e == GFARM_ERR_NETWORK_DROPPED_CONNECTION_ON_RESET ||
	    e == GFARM_ERR_HOST_IS_DOWN ||
	    e == GFARM_ERR_NO_ROUTE_TO_HOST ||
	    e == GFARM_ERR_CONNECTION_TIMED_OUT ||
	    e == GFARM_ERR_CONNECTION_REFUSED ||
	    e == GFARM_ERR_CONNECTION_RESET_BY_PEER ||
	    e == GFARM_ERR_UNEXPECTED_EOF);
}

int
gfs_client_connection_fd(struct gfs_connection *gfs_server)
{
	return (xxx_connection_fd(gfs_server->conn));
}

enum gfarm_auth_method
gfs_client_connection_auth_method(struct gfs_connection *gfs_server)
{
	return (gfs_server->auth_method);
}

const char *
gfs_client_hostname(struct gfs_connection *gfs_server)
{
	return (gfs_server->hostname);
}


static void
gfs_client_uncached_connection_created(struct gfs_connection *gfs_server)
{
	gfs_server->hash_entry = NULL;
	gfs_server->next = gfs_server->prev = NULL;
}

static void
gfs_client_cached_connection_created(struct gfs_connection *gfs_server,
	struct gfarm_hash_entry *entry)
{
	*(struct gfs_connection **)gfarm_hash_entry_data(entry) = gfs_server;
	gfs_server->hash_entry = entry;

	gfs_server->next = connection_list_head.next;
	gfs_server->prev = &connection_list_head;
	connection_list_head.next->prev = gfs_server;
	connection_list_head.next = gfs_server;
}

static void
gfs_client_cached_connection_removed(struct gfs_connection *gfs_server)
{
	gfs_server->hash_entry = NULL;
	gfs_server->next->prev = gfs_server->prev;
	gfs_server->prev->next = gfs_server->next;
}

static struct gfs_connection *
gfs_client_purge_iterator(struct gfarm_hash_iterator *it)
{
	struct gfarm_hash_entry *entry;
	struct gfs_connection *gfs_server;

	entry = gfarm_hash_iterator_access(it);
	gfs_server = *(struct gfs_connection **)gfarm_hash_entry_data(entry);
	gfp_conn_hash_iterator_purge(it);
	gfs_client_cached_connection_removed(gfs_server);
	return (gfs_server);
}

static void
gfs_client_purge_connection(struct gfs_connection *gfs_server)
{
	/*
	 * This must be called before gfs_client_cached_connection_removed(),
	 * becasue gfs_client_cached_connection_removed() breaks hash_entry.
	 */
	gfp_conn_hash_purge(gfs_server_hashtab, gfs_server->hash_entry);

	gfs_client_cached_connection_removed(gfs_server);
}

void
gfs_client_purge_from_cache(struct gfs_connection *gfs_server)
{
	if (!gfs_client_connection_is_cached(gfs_server))
		return;

	gfs_client_purge_connection(gfs_server);
}

/* update the LRU list to mark this gfs_server recently used */
static void
gfs_client_connection_used(struct gfs_connection *gfs_server)
{
	if (!gfs_client_connection_is_cached(gfs_server))
		return;

	gfs_server->next->prev = gfs_server->prev;
	gfs_server->prev->next = gfs_server->next;

	gfs_server->next = connection_list_head.next;
	gfs_server->prev = &connection_list_head;
	connection_list_head.next->prev = gfs_server;
	connection_list_head.next = gfs_server;
}

static void
gfs_client_connection_refered(struct gfs_connection *gfs_server)
{
	if (gfs_server->acquired == 0) /* now this isn't free */
		--free_connections;
	gfs_server->acquired++;
	gfs_client_connection_used(gfs_server);
}

static void
gfs_client_connection_gc_internal(int free_target)
{
	struct gfs_connection *gfs_server, *prev;

	/* search least recently used connection */
	for (gfs_server = connection_list_head.prev;
	    free_connections > free_target; gfs_server = prev) {
		prev = gfs_server->prev;

		if (gfs_server == &connection_list_head) {
			gflog_error("free connections/target = %d/%d",
			    free_connections, free_target);
			gflog_error("But no free connection is found.");
			gflog_error("This shouldn't happen");
			abort();
		}

		if (gfs_server->acquired <= 0) {
			/* abandon this free connection */
			gfs_client_purge_connection(gfs_server);
			gfs_client_disconnect(gfs_server);
			--free_connections;
		}
	}
}

static char *
connect_wait(int s)
{
	fd_set wset;
	struct timeval timeout;
	int rv, error;
	socklen_t error_size;

	FD_ZERO(&wset);
	FD_SET(s, &wset);
	timeout.tv_sec = GFS_CLIENT_CONNECT_TIMEOUT;
	timeout.tv_usec = 0;

	rv = select(s + 1, NULL, &wset, NULL, &timeout);
	if (rv == 0)
		return (gfarm_errno_to_error(ETIMEDOUT));
	if (rv < 0)
		return (gfarm_errno_to_error(errno));

	error_size = sizeof(error);
	rv = getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &error_size);
	if (rv == -1)
		return (gfarm_errno_to_error(errno));
	if (error != 0)
		return (gfarm_errno_to_error(errno));
	return (GFARM_ERR_NO_ERROR);
}

void
gfs_client_connection_gc(void)
{
	gfs_client_connection_gc_internal(0);
}

static int
sockaddr_is_local(const struct sockaddr *peer_addr)
{
	static int self_ip_asked = 0;
	static int self_ip_count = 0;
	static struct in_addr *self_ip_list;

	const struct sockaddr_in *peer_in;
	int i;

	if (!self_ip_asked) {
		self_ip_asked = 1;
		if (gfarm_get_ip_addresses(&self_ip_count, &self_ip_list) !=
		    NULL) {
			/* self_ip_count remains 0 */
			return (0);
		}
	}
	if (peer_addr->sa_family != AF_INET)
		return (0);
	peer_in = (const struct sockaddr_in *)peer_addr;
	/* XXX if there are lots of IP address on this host, this is slow */
	for (i = 0; i < self_ip_count; i++) {
		if (peer_in->sin_addr.s_addr == self_ip_list[i].s_addr)
			return (1);
	}
	return (0);
}

static char *
gfs_client_connect_unix(struct sockaddr *peer_addr, int port, int *sockp)
{
	int rv, sock;
	struct sockaddr_un peer_un;
	struct sockaddr_in *peer_in;
	socklen_t socklen;

	assert(peer_addr->sa_family == AF_INET);
	peer_in = (struct sockaddr_in *)peer_addr;

	sock = socket(PF_UNIX, SOCK_STREAM, 0);
	if (sock == -1 && (errno == ENFILE || errno == EMFILE)) {
		gfs_client_connection_gc();
		sock = socket(PF_UNIX, SOCK_STREAM, 0);
	}
	if (sock == -1)
		return (gfarm_errno_to_error(errno));

	memset(&peer_un, 0, sizeof(peer_un));
	/* XXX inet_ntoa() is not MT-safe on some platforms */
	socklen = snprintf(peer_un.sun_path, sizeof(peer_un.sun_path),
	    GFSD_LOCAL_SOCKET_NAME, inet_ntoa(peer_in->sin_addr), port);
	peer_un.sun_family = AF_UNIX;
#ifdef SUN_LEN /* derived from 4.4BSD */
	socklen = SUN_LEN(&peer_un);
#else
	socklen += sizeof(peer_un) - sizeof(peer_un.sun_path);
#endif
	rv = connect(sock, (struct sockaddr *)&peer_un, socklen);
	if (rv == -1) {
		close(sock);
		if (errno != ENOENT)
			return (gfarm_errno_to_error(errno));

		/* older gfsd doesn't support UNIX connection, try INET */
		sock = -1;
	}
	*sockp = sock;
	return (GFARM_ERR_NO_ERROR);
}

static char *
gfs_client_connect_inet(const char *canonical_hostname,
	struct sockaddr *peer_addr, int port,
	int *connection_in_progress_p, int *sockp)
{
	int rv, sock;

	sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock == -1 && (errno == ENFILE || errno == EMFILE)) {
		gfs_client_connection_gc();
		sock = socket(PF_INET, SOCK_STREAM, 0);
	}
	if (sock == -1)
		return (gfarm_errno_to_error(errno));

	/* XXX - how to report setsockopt(2) failure ? */
	gfarm_sockopt_apply_by_name_addr(sock, canonical_hostname, peer_addr);

	/*
	 * this fcntl should never fail, or even if this fails, that's OK
	 * because its only effect is that TCP timeout becomes longer.
	 */
	fcntl(sock, F_SETFL, O_NONBLOCK);
	rv = connect(sock, peer_addr, sizeof(*peer_addr));
	if (rv < 0) {
		if (errno != EINPROGRESS) {
			close(sock);
			return (gfarm_errno_to_error(errno));
		}
		*connection_in_progress_p = 1;
	} else {
		*connection_in_progress_p = 0;
	}
	fcntl(sock, F_SETFL, 0); /* clear O_NONBLOCK, this should never fail */
	*sockp = sock;
	return (GFARM_ERR_NO_ERROR);
}

/*
 * The struct gfs_connection, which is acquired by this function, must be
 * freed by gfs_client_connection_free().
 */
static char *
gfs_client_connection_alloc(const char *canonical_hostname,
	struct sockaddr *peer_addr, int port,
	int *connection_in_progress_p, struct gfs_connection **gfs_serverp)
{
	char *e;
	struct gfs_connection *gfs_server;
	int connection_in_progress, sock = -1;

#ifdef __GNUC__ /* workaround gcc warning: may be used uninitialized */
	connection_in_progress = 0;
#endif
	if (!gfsd_version_1_2_or_earlier && sockaddr_is_local(peer_addr)) {
		e = gfs_client_connect_unix(peer_addr, port, &sock);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		if (sock == -1) /* older gfsd only supports INET connection */
			gfsd_version_1_2_or_earlier = 1;
		else
			connection_in_progress = 0;
	}
	if (sock == -1) {
		e = gfs_client_connect_inet(canonical_hostname,
		    peer_addr, port, &connection_in_progress, &sock);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
	}
	fcntl(sock, F_SETFD, 1); /* automatically close() on exec(2) */

	GFARM_MALLOC(gfs_server);
	if (gfs_server == NULL) {
		close(sock);
		return (GFARM_ERR_NO_MEMORY);
	}
	e = xxx_socket_connection_new(sock, &gfs_server->conn);
	if (e != GFARM_ERR_NO_ERROR) {
		free(gfs_server);
		close(sock);
		return (e);
	}
	gfs_server->hostname = strdup(canonical_hostname);
	if (gfs_server->hostname == NULL) {
		e = xxx_connection_free(gfs_server->conn);
		free(gfs_server);
		return (GFARM_ERR_NO_MEMORY);
	}
	gfs_server->context = NULL;
	gfs_server->acquired = 1;
	gfs_server->opened = 0;

	*connection_in_progress_p = connection_in_progress;
	*gfs_serverp = gfs_server;
	return (GFARM_ERR_NO_ERROR);
}

/*
 * Shouldn't be used for a gfs_connection created by gfs_client_connect().
 * Should be used for a gfs_connection which was created by
 * gfs_client_connection_acquire().
 */
char *
gfs_client_connection_free(struct gfs_connection *gfs_server)
{
	if (--gfs_server->acquired > 0)
		return (GFARM_ERR_NO_ERROR); /* shouln't be freed */
#if 0	/*
	 * This happens if a descriptor couldn't be closed
	 * because its gfsd was already dead.
	 */
	/* sanity check */
	if (gfs_server->opened > 0) {
		gflog_error("gfs_server->acquired = %d, but\n",
		    gfs_server->acquired);
		gflog_error("gfs_server->opened = %d.\n",
		    gfs_server->opened);
		gflog_error("Something is forgetting to close a file\n");
		abort();
	}
#endif
	if (!gfs_client_connection_is_cached(gfs_server)) {/* already purged */
		gfs_client_disconnect(gfs_server);
		return (GFARM_ERR_NO_ERROR);
	}

	++free_connections;
	gfs_client_connection_gc_internal(gfarm_gfsd_connection_cache);

	return (GFARM_ERR_NO_ERROR);
}

static char *
gfs_client_connection_dispose(struct gfs_connection *gfs_server)
{
	char *e = xxx_connection_free(gfs_server->conn);

	free(gfs_server->hostname);
	/* XXX - gfs_server->context should be NULL here */
	free(gfs_server);
	return (e);
}

void
gfs_client_terminate(void)
{
	struct gfarm_hash_iterator it;
	struct gfs_connection *gfs_server;

	if (gfs_server_hashtab == NULL)
		return;
	for (gfarm_hash_iterator_begin(gfs_server_hashtab, &it);
	     !gfarm_hash_iterator_is_end(&it); ) {
		gfs_server = gfs_client_purge_iterator(&it);
		gfs_client_connection_dispose(gfs_server);
	}
	gfarm_hash_table_free(gfs_server_hashtab);
	gfs_server_hashtab = NULL;
}

/*
 * `hostname' to `addr' conversion really should be done in this function,
 * rather than a caller of this function.
 * but currently gfsd cannot access gfmd, and we need to access gfmd to
 * resolve hostname. (to check host_alias for "address_use" directive.)
 */
char *
gfs_client_connection_acquire(const char *canonical_hostname,
	struct sockaddr *peer_addr, struct gfs_connection **gfs_serverp)
{
	char *e;
	struct gfarm_hash_entry *entry;
	struct gfs_connection *gfs_server;
	int created, connection_in_progress;

	e = gfp_conn_hash_enter(&gfs_server_hashtab, SERVER_HASHTAB_SIZE,
	    sizeof(gfs_server),
	    canonical_hostname, gfarm_spool_server_port,
	    gfarm_get_global_username(),
	    &entry, &created);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (!created) {
		gfs_server =
		    *(struct gfs_connection **)gfarm_hash_entry_data(entry);
		gfs_client_connection_refered(gfs_server);
	} else {
		e = gfs_client_connection_alloc(canonical_hostname,
		    peer_addr, gfarm_spool_server_port,
		    &connection_in_progress, &gfs_server);
		if (e != GFARM_ERR_NO_ERROR) {
			gfp_conn_hash_purge(gfs_server_hashtab, entry);
			return (e);
		}
		if (connection_in_progress) {
			e = connect_wait(xxx_connection_fd(gfs_server->conn));
			if (e != GFARM_ERR_NO_ERROR) {
				gfp_conn_hash_purge(gfs_server_hashtab, entry);
				gfs_client_connection_dispose(gfs_server);
				return (e);
			}
		}
		e = gfarm_auth_request(gfs_server->conn, GFS_SERVICE_TAG,
		    gfs_server->hostname, peer_addr,
		    &gfs_server->auth_method);
		if (e != GFARM_ERR_NO_ERROR) {
			gfp_conn_hash_purge(gfs_server_hashtab, entry);
			gfs_client_connection_dispose(gfs_server);
			return (e);
		}
		gfs_client_cached_connection_created(gfs_server, entry);
	}
	*gfs_serverp = gfs_server;
	return (GFARM_ERR_NO_ERROR);
}

/*
 * The struct gfs_connection, which is acquired by this function, must be
 * freed by gfs_client_disconnect().
 *
 * XXX FIXME
 * `hostname' to `addr' conversion really should be done in this function,
 * rather than a caller of this function.
 * but currently gfsd cannot access gfmd, and we need to access gfmd to
 * resolve hostname. (to check host_alias for "address_use" directive.)
 */
char *
gfs_client_connect(const char *canonical_hostname, struct sockaddr *peer_addr,
	struct gfs_connection **gfs_serverp)
{
	char *e;
	struct gfs_connection *gfs_server;
	int connection_in_progress;

	e = gfs_client_connection_alloc(canonical_hostname,
	    peer_addr, gfarm_spool_server_port,
	    &connection_in_progress, &gfs_server);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (connection_in_progress)
		e = connect_wait(xxx_connection_fd(gfs_server->conn));
	if (e == GFARM_ERR_NO_ERROR)
		e = gfarm_auth_request(gfs_server->conn, GFS_SERVICE_TAG,
		    gfs_server->hostname, peer_addr,
		    &gfs_server->auth_method);
	if (e != GFARM_ERR_NO_ERROR) {
		gfs_client_connection_dispose(gfs_server);
		return (e);
	}

	gfs_client_uncached_connection_created(gfs_server);
	*gfs_serverp = gfs_server;
	return (NULL);
}

/*
 * Should be used for a gfs_connection created by gfs_client_connect().
 * Shouldn't be used for a gfs_connection which was acquired by
 * gfs_client_connection_acquire().
 */
char *
gfs_client_disconnect(struct gfs_connection *gfs_server)
{
	if (gfs_client_connection_is_cached(gfs_server)) {
		gflog_error("gfs_client_disconnect: programming error");
		abort();
	}
	return (gfs_client_connection_dispose(gfs_server));
}

#if 0 /* this function is currently not used */
/*
 * NOTE:
 * The caller of this function should obey the following rule:
 * if this function returns error:
 * 	The caller usually should call gfs_client_disconnect(gfs_server).
 * otherwise (i.e. success case):
 * 	The caller must not use the variable `gfs_server' any more.
 */
char *
gfs_client_connection_enter_cache(struct gfs_connection *gfs_server)
{
	char *e;
	struct gfarm_hash_entry *entry;
	int created;

	if (gfs_client_connection_is_cached(gfs_server)) {
		gflog_error("gfs_client_connection_enter_cache: "
		    "programming error");
		abort();
	}
	e = gfp_conn_hash_enter(&gfs_server_hashtab, SERVER_HASHTAB_SIZE,
	    sizeof(gfs_server),
	    gfs_server->hostname, gfarm_spool_server_port,
	    gfarm_get_global_username(),
	    &entry, &created);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (!created)
		return (GFARM_ERR_ALREADY_EXISTS)

	gfs_client_cached_connection_created(gfs_server, entry);
	gfs_client_connection_free(gfs_server); /* not acquired */
	return (GFARM_ERR_NO_ERROR);
}
#endif

/*
 * multiplexed version of gfs_client_connect() for parallel authentication
 */

struct gfs_client_connect_state {
	struct gfarm_eventqueue *q;
	struct gfarm_event *writable;
	struct sockaddr peer_addr;
	void (*continuation)(void *);
	void *closure;

	struct gfs_connection *gfs_server;

	struct gfarm_auth_request_state *auth_state;

	/* results */
	char *error;
};

static void
gfs_client_connect_end_auth(void *closure)
{
	struct gfs_client_connect_state *state = closure;

	state->error = gfarm_auth_result_multiplexed(
	    state->auth_state,
	    &state->gfs_server->auth_method);
	if (state->continuation != NULL)
		(*state->continuation)(state->closure);
}

static void
gfs_client_connect_start_auth(int events, int fd, void *closure,
	const struct timeval *t)
{
	struct gfs_client_connect_state *state = closure;
	int error;
	socklen_t error_size = sizeof(error);
	int rv = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size);

	if (rv == -1) { /* Solaris, see UNP by rstevens */
		state->error = gfarm_errno_to_error(errno);
	} else if (error != 0) {
		state->error = gfarm_errno_to_error(error);
	} else { /* successfully connected */
		state->error = gfarm_auth_request_multiplexed(state->q,
		    state->gfs_server->conn, GFS_SERVICE_TAG,
		    state->gfs_server->hostname, &state->peer_addr,
		    gfs_client_connect_end_auth, state,
		    &state->auth_state);
		if (state->error == NULL) {
			/*
			 * call auth_request,
			 * then go to gfs_client_connect_end_auth()
			 */
			return;
		}
	}
	if (state->continuation != NULL)
		(*state->continuation)(state->closure);
}

char *
gfs_client_connect_request_multiplexed(struct gfarm_eventqueue *q,
	const char *canonical_hostname, struct sockaddr *peer_addr,
	void (*continuation)(void *), void *closure,
	struct gfs_client_connect_state **statepp)
{
	char *e;
	int rv;
	struct gfs_connection *gfs_server;
	struct gfs_client_connect_state *state;
	int connection_in_progress;

	/* clone of gfs_client_connect() */

	e = gfs_client_connection_alloc(canonical_hostname,
	    peer_addr, gfarm_spool_server_port,
	    &connection_in_progress, &gfs_server);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	GFARM_MALLOC(state);
	if (state == NULL) {
		gfs_client_connection_dispose(gfs_server);
		return (GFARM_ERR_NO_MEMORY);
	}
	state->q = q;
	state->peer_addr = *peer_addr;
	state->continuation = continuation;
	state->closure = closure;
	state->gfs_server = gfs_server;
	state->auth_state = NULL;
	state->error = NULL;
	if (connection_in_progress) {
		state->writable = gfarm_fd_event_alloc(GFARM_EVENT_WRITE,
		    xxx_connection_fd(gfs_server->conn),
		    gfs_client_connect_start_auth, state);
		if (state->writable == NULL) {
			e = GFARM_ERR_NO_MEMORY;
		} else if ((rv = gfarm_eventqueue_add_event(q, state->writable,
		    NULL)) == 0) {
			*statepp = state;
			/* go to gfs_client_connect_start_auth() */
			return (NULL);
		} else {
			e = gfarm_errno_to_error(rv);
			gfarm_event_free(state->writable);
		}
	} else {
		state->writable = NULL;
		e = gfarm_auth_request_multiplexed(q,
		    gfs_server->conn, GFS_SERVICE_TAG,
		    gfs_server->hostname, &state->peer_addr,
		    gfs_client_connect_end_auth, state,
		    &state->auth_state);
		if (e == NULL) {
			*statepp = state;
			/*
			 * call gfarm_auth_request,
			 * then go to gfs_client_connect_end_auth()
			 */
			return (NULL);
		}
	}
	free(state);
	gfs_client_connection_dispose(gfs_server);
	return (e);
}

char *
gfs_client_connect_result_multiplexed(
	struct gfs_client_connect_state *state,
	struct gfs_connection **gfs_serverp)
{
	char *e = state->error;
	struct gfs_connection *gfs_server = state->gfs_server;

	if (state->writable != NULL)
		gfarm_event_free(state->writable);
	free(state);
	if (e != NULL) {
		gfs_client_connection_dispose(gfs_server);
		return (e);
	}

	gfs_client_uncached_connection_created(gfs_server);

	*gfs_serverp = gfs_server;
	return (NULL);
}

/*
 * gfs_client RPC
 */

static int
gfarm_fd_receive_message(int fd, void *buf, size_t size,
	int fdc, int *fdv)
{
	char *buffer = buf;
	int i, rv;
	struct iovec iov[1];
	struct msghdr msg;
#ifdef HAVE_MSG_CONTROL /* 4.3BSD Reno or later */
	struct {
		struct cmsghdr hdr;
		char data[CMSG_SPACE(sizeof(*fdv) * GFSD_MAX_PASSING_FD)
			  - sizeof(struct cmsghdr)];
	} cmsg;

	if (fdc > GFSD_MAX_PASSING_FD) {
#if 0
		fprintf(stderr, "gfarm_fd_receive_message(%s): "
			"fd count %d > %d\n", fdc, GFSD_MAX_PASSING_FD);
#endif
		return (EINVAL);
	}
#endif

	while (size > 0) {
		iov[0].iov_base = buffer;
		iov[0].iov_len = size;
		msg.msg_iov = iov;
		msg.msg_iovlen = 1;
		msg.msg_name = NULL;
		msg.msg_namelen = 0;
#ifndef HAVE_MSG_CONTROL
		if (fdc > 0) {
			msg.msg_accrights = (caddr_t)fdv;
			msg.msg_accrightslen = sizeof(*fdv) * fdc;
			for (i = 0; i < fdc; i++)
				fdv[i] = -1;
		} else {
			msg.msg_accrights = NULL;
			msg.msg_accrightslen = 0;
		}
#else /* 4.3BSD Reno or later */
		if (fdc > 0) {
			msg.msg_control = (caddr_t)&cmsg.hdr;
			msg.msg_controllen = CMSG_SPACE(sizeof(*fdv) * fdc);
			memset(msg.msg_control, 0, msg.msg_controllen);
			for (i = 0; i < fdc; i++)
				((int *)CMSG_DATA(&cmsg.hdr))[i] = -1;
		} else {
			msg.msg_control = NULL;
			msg.msg_controllen = 0;
		}
#endif
		rv = recvmsg(fd, &msg, 0);
		if (rv == -1) {
			if (errno == EINTR)
				continue;
			return (errno); /* failure */
		} else if (rv == 0) {
			return (-1); /* EOF */
		}
#ifdef HAVE_MSG_CONTROL /* 4.3BSD Reno or later */
		if (fdc > 0) {
			if (msg.msg_controllen !=
			    CMSG_SPACE(sizeof(*fdv) * fdc) ||
			    cmsg.hdr.cmsg_len !=
			    CMSG_LEN(sizeof(*fdv) * fdc) ||
			    cmsg.hdr.cmsg_level != SOL_SOCKET ||
			    cmsg.hdr.cmsg_type != SCM_RIGHTS) {
#if 0
				fprintf(stderr,
					"gfarm_fd_receive_message():"
					" descriptor not passed"
					" msg_controllen: %d (%d),"
					" cmsg_len: %d (%d),"
					" cmsg_level: %d,"
					" cmsg_type: %d\n",
					msg.msg_controllen,
					CMSG_SPACE(sizeof(*fdv) * fdc),
					cmsg.hdr.cmsg_len,
					CMSG_LEN(sizeof(*fdv) * fdc),
					cmsg.hdr.cmsg_level,
					cmsg.hdr.cmsg_type);
#endif
			}
			for (i = 0; i < fdc; i++)
				fdv[i] = ((int *)CMSG_DATA(&cmsg.hdr))[i];
		}
#endif
		fdc = 0; fdv = NULL;
		buffer += rv;
		size -= rv;
	}
	return (0); /* success */
}

char *
gfs_client_rpc_request(struct gfs_connection *gfs_server, int command,
		       char *format, ...)
{
	va_list ap;
	char *e;

	va_start(ap, format);
	e = xxx_proto_vrpc_request(gfs_server->conn, command, &format, &ap);
	va_end(ap);
	if (gfs_client_is_connection_error(e)) {
		if (gfs_client_hook_for_connection_error != NULL)
			(*gfs_client_hook_for_connection_error)(
			    gfs_client_hostname(gfs_server));
		gfs_client_purge_from_cache(gfs_server);
	}
	return (e);
}

char *
gfs_client_rpc_result(struct gfs_connection *gfs_server, int just,
		      char *format, ...)
{
	va_list ap;
	char *e;
	int error;

	va_start(ap, format);
	e = xxx_proto_vrpc_result(gfs_server->conn, just,
				  &error, &format, &ap);
	va_end(ap);

	if (gfs_client_is_connection_error(e)) {
		if (gfs_client_hook_for_connection_error != NULL)
			(*gfs_client_hook_for_connection_error)(
			    gfs_client_hostname(gfs_server));
		gfs_client_purge_from_cache(gfs_server);
	}
	if (e != NULL)
		return (e);
	if (error != 0)
		return (gfs_proto_error_string(error));
	return (NULL);
}

char *
gfs_client_rpc(struct gfs_connection *gfs_server, int just, int command,
	       char *format, ...)
{
	va_list ap;
	char *e;
	int error;

	va_start(ap, format);
	e = xxx_proto_vrpc(gfs_server->conn, just,
			   command, &error, &format, &ap);
	va_end(ap);

	if (gfs_client_is_connection_error(e)) {
		if (gfs_client_hook_for_connection_error != NULL)
			(*gfs_client_hook_for_connection_error)(
			    gfs_client_hostname(gfs_server));
		gfs_client_purge_from_cache(gfs_server);
	}
	if (e != NULL)
		return (e);
	if (error != 0)
		return (gfs_proto_error_string(error));
	return (NULL);
}

char *
gfs_client_open(struct gfs_connection *gfs_server,
		char *gfarm_file, gfarm_int32_t flag, gfarm_int32_t mode,
		gfarm_int32_t *fdp)
{
	char *e = gfs_client_rpc(gfs_server, 0,
	    GFS_PROTO_OPEN, "sii/i", gfarm_file, flag, mode, fdp);

	gfs_client_connection_used(gfs_server);
	if (e == GFARM_ERR_NO_ERROR)
		++gfs_server->opened;
	return (e);
}

/*
 * This function is prepared for backward compatibility to access old
 * gfsd that does not support GFS_PROTO_OPEN_LOCAL.
 */
static char *
gfs_client_open_direct(char *gfarm_file, gfarm_int32_t flag,
	gfarm_int32_t mode, gfarm_int32_t *fdp)
{
	char *local_path, *e;
	int local_fd, local_flag, saved_errno;
	mode_t saved_umask;

	local_flag = gfs_open_flags_localize(flag);
	if (local_flag == -1)
		return (GFARM_ERR_INVALID_ARGUMENT);

	e = gfarm_path_localize(gfarm_file, &local_path);
	if (e != NULL)
		return (e);

	saved_umask = umask(0);
	local_fd = open(local_path, local_flag, mode & GFARM_S_ALLPERM);
	saved_errno = errno;
	umask(saved_umask);
	free(local_path);
	if (local_fd == -1)
		return (gfarm_errno_to_error(saved_errno));
	*fdp = local_fd;
	return (NULL);
}

char *
gfs_client_open_local(struct gfs_connection *gfs_server,
	char *gfarm_file, gfarm_int32_t flag, gfarm_int32_t mode,
	gfarm_int32_t *fdp)
{
	char *e;
	int rv, local_fd, err;

	/*
	 * We don't do ++gfs_server->opened in this function,
	 * because the server doesn't remember the descriptor.
	 */
	gfs_client_connection_used(gfs_server);

	if (gfsd_version_1_2_or_earlier) {
		/* try to open directly for backward compatibility */
		return (gfs_client_open_direct(gfarm_file, flag, mode, fdp));
	}
	/* we have to set `just' flag here */
	e = gfs_client_rpc(gfs_server, 1, GFS_PROTO_OPEN_LOCAL, "sii/",
		gfarm_file, flag, mode);
	if (e != NULL)
		return (e);

	/* layering violation, but... */
	rv = gfarm_fd_receive_message(xxx_connection_fd(gfs_server->conn),
	    &err, sizeof(int), 1, &local_fd);
	if (rv == -1)
		return (GFARM_ERR_UNEXPECTED_EOF);
	if (rv != 0)
		return (gfarm_errno_to_error(rv));
	/* both `err' and `local_fd` are passed by using host byte order. */
	if (err != 0)
		return (gfs_proto_error_string(err));
	*fdp = local_fd;
	return (NULL);
}

char *
gfs_client_close(struct gfs_connection *gfs_server, gfarm_int32_t fd)
{
	char *e = gfs_client_rpc(gfs_server, 0, GFS_PROTO_CLOSE, "i/", fd);

	gfs_client_connection_used(gfs_server);

	if (e == GFARM_ERR_NO_ERROR)
		--gfs_server->opened;
	return (e);
}

char *
gfs_client_seek(struct gfs_connection *gfs_server,
		gfarm_int32_t fd, file_offset_t offset, gfarm_int32_t whence,
		file_offset_t *resultp)
{
	file_offset_t dummy;

	gfs_client_connection_used(gfs_server);

	if (resultp == NULL)
		resultp = &dummy;
	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_SEEK, "ioi/o",
			       fd, offset, whence, resultp));
}

char *
gfs_client_ftruncate(struct gfs_connection *gfs_server,
		    gfarm_int32_t fd, file_offset_t length)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_FTRUNCATE, "io/",
			       fd, length));
}

char *
gfs_client_read(struct gfs_connection *gfs_server,
		gfarm_int32_t fd, void *buffer, size_t size, size_t *np)
{
	char *e;

	gfs_client_connection_used(gfs_server);

	e = gfs_client_rpc(gfs_server, 0, GFS_PROTO_READ, "ii/b",
			   fd, (int)size,
			   size, np, buffer);
	if (e != NULL)
		return (e);
	if (*np > size)
		return ("gfs_pio_read: protocol error");
	return (NULL);
}

char *
gfs_client_write(struct gfs_connection *gfs_server,
		 gfarm_int32_t fd, const void *buffer, size_t size, size_t *np)
{
	char *e;
	gfarm_int32_t n; /* size_t may be 64bit */

	gfs_client_connection_used(gfs_server);

	e = gfs_client_rpc(gfs_server, 0, GFS_PROTO_WRITE, "ib/i",
			   fd, size, buffer,
			   &n);
	if (e != NULL)
		return (e);
	*np = n;
	if (n > size)
		return ("gfs_pio_write: protocol error");
	return (NULL);
}

char *
gfs_client_fsync(struct gfs_connection *gfs_server,
		 gfarm_int32_t fd, gfarm_int32_t operation)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_FSYNC, "ii/",
			       fd, operation));
}

char *
gfs_client_link(struct gfs_connection *gfs_server, char *from, char *to)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_LINK, "ss/",
			       from, to));
}

char *
gfs_client_unlink(struct gfs_connection *gfs_server, char *path)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_UNLINK, "s/", path));
}

char *
gfs_client_rename(struct gfs_connection *gfs_server, char *from, char *to)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_RENAME, "ss/",
			       from, to));
}

char *
gfs_client_chdir(struct gfs_connection *gfs_server, char *dir)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_CHDIR, "s/", dir));
}

char *
gfs_client_mkdir(struct gfs_connection *gfs_server,
		 char *dir, gfarm_int32_t mode)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_MKDIR, "si/",
			       dir, mode));
}

char *
gfs_client_rmdir(struct gfs_connection *gfs_server, char *dir)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_RMDIR, "s/", dir));
}

char *
gfs_client_chmod(struct gfs_connection *gfs_server,
		 char *path, gfarm_int32_t mode)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_CHMOD, "si/",
			       path, mode));
}

char *
gfs_client_chgrp(struct gfs_connection *gfs_server, char *path, char *group)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_CHGRP, "ss/",
			       path, group));
}

char *
gfs_client_fstat(struct gfs_connection *gfs_server, gfarm_int32_t fd,
	struct stat *st)
{
	file_offset_t size;
	char *e;

	gfs_client_connection_used(gfs_server);

	e = gfs_client_rpc(gfs_server, 0, GFS_PROTO_FSTAT, "i/iioiii", fd,
			   &st->st_mode, &st->st_nlink, &size,
			   &st->st_atime, &st->st_mtime, &st->st_ctime);
	if (e == NULL)
		st->st_size = size;
	return (e);
}

char *
gfs_client_exist(struct gfs_connection *gfs_server, char *path)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_EXIST, "s/",
			       path));
}

char *
gfs_client_digest(struct gfs_connection *gfs_server,
		  int fd, char *digest_type,
		  size_t digest_size, size_t *digest_length,
		  unsigned char *digest, file_offset_t *filesize)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_DIGEST, "is/bo",
			       fd, digest_type,
			       digest_size, digest_length, digest, filesize));
}

char *
gfs_client_get_spool_root(struct gfs_connection *gfs_server,
			  char **spool_rootp)
{
	char *e = gfs_client_rpc(gfs_server, 0, GFS_PROTO_GET_SPOOL_ROOT, "/s",
				 spool_rootp);

	gfs_client_connection_used(gfs_server);

	if (e != NULL)
		return (e);
	return (*spool_rootp == NULL ? GFARM_ERR_NO_MEMORY : NULL);
}

char *
gfs_client_statfs(struct gfs_connection *gfs_server, char *path,
	gfarm_int32_t *bsizep,
	file_offset_t *blocksp, file_offset_t *bfreep, file_offset_t *bavailp,
	file_offset_t *filesp, file_offset_t *ffreep, file_offset_t *favailp)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_STATFS, "s/ioooooo",
			       path, bsizep,
			       blocksp, bfreep, bavailp,
			       filesp, ffreep, favailp));
}

/*
 * multiplexed version of gfs_client_statfs()
 */
struct gfs_client_statfs_state {
	struct gfarm_eventqueue *q;
	struct gfarm_event *writable, *readable;
	void (*continuation)(void *);
	void *closure;
	struct gfs_connection *gfs_server;
	char *path;

	/* results */
	char *error;
	gfarm_int32_t bsize;
	file_offset_t blocks, bfree, bavail;
	file_offset_t files, ffree, favail;
};

static void
gfs_client_statfs_send_request(int events, int fd, void *closure,
	const struct timeval *t)
{
	struct gfs_client_statfs_state *state = closure;
	int rv;
	struct timeval timeout;

	state->error = gfs_client_rpc_request(state->gfs_server,
	    GFS_PROTO_STATFS, "s", state->path);
	if (state->error == NULL &&
	    (state->error = xxx_proto_flush(state->gfs_server->conn)) == NULL){
		timeout.tv_sec = GFS_CLIENT_COMMAND_TIMEOUT;
		timeout.tv_usec = 0;
		if ((rv = gfarm_eventqueue_add_event(state->q, state->readable,
		    &timeout)) == 0) {
			/* go to gfs_client_statfs_recv_result() */
			return;
		}
		state->error = gfarm_errno_to_error(rv);
	}
	if (state->continuation != NULL)
		(*state->continuation)(state->closure);
}

static void
gfs_client_statfs_recv_result(int events, int fd, void *closure,
	const struct timeval *t)
{
	struct gfs_client_statfs_state *state = closure;

	if ((events & GFARM_EVENT_TIMEOUT) != 0) {
		assert(events == GFARM_EVENT_TIMEOUT);
		state->error = GFARM_ERR_CONNECTION_TIMED_OUT;
	} else {
		assert(events == GFARM_EVENT_READ);
		state->error = gfs_client_rpc_result(state->gfs_server, 0,
		    "ioooooo", &state->bsize,
		    &state->blocks, &state->bfree, &state->bavail,
		    &state->files, &state->ffree, &state->favail);
	}
	if (state->continuation != NULL)
		(*state->continuation)(state->closure);
}

char *
gfs_client_statfs_request_multiplexed(struct gfarm_eventqueue *q,
	struct gfs_connection *gfs_server, char *path,
	void (*continuation)(void *), void *closure,
	struct gfs_client_statfs_state **statepp)
{
	char *e;
	int rv;
	struct gfs_client_statfs_state *state;

	gfs_client_connection_used(gfs_server);

	GFARM_MALLOC(state);
	if (state == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		goto error_return;
	}

	state->q = q;
	state->continuation = continuation;
	state->closure = closure;
	state->gfs_server = gfs_server;
	state->path = path;
	state->error = NULL;
	state->writable = gfarm_fd_event_alloc(GFARM_EVENT_WRITE,
	    gfs_client_connection_fd(gfs_server),
	    gfs_client_statfs_send_request, state);
	if (state->writable == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		goto error_free_state;
	}
	/*
	 * We cannot use two independent events (i.e. a fd_event with
	 * GFARM_EVENT_READ flag and a timer_event) here, because
	 * it's possible that both event handlers are called at once.
	 */
	state->readable = gfarm_fd_event_alloc(
	    GFARM_EVENT_READ|GFARM_EVENT_TIMEOUT,
	    gfs_client_connection_fd(gfs_server),
	    gfs_client_statfs_recv_result, state);
	if (state->readable == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		goto error_free_writable;
	}
	/* go to gfs_client_statfs_send_request() */
	rv = gfarm_eventqueue_add_event(q, state->writable, NULL);
	if (rv != 0) {
		e = gfarm_errno_to_error(rv);
		goto error_free_readable;
	}
	*statepp = state;
	return (NULL);
		
error_free_readable:
	gfarm_event_free(state->readable);
error_free_writable:
	gfarm_event_free(state->writable);
error_free_state:
	free(state);
error_return:
	return (e);
}

char *
gfs_client_statfs_result_multiplexed(struct gfs_client_statfs_state *state,
	gfarm_int32_t *bsizep,
	file_offset_t *blocksp, file_offset_t *bfreep, file_offset_t *bavailp,
	file_offset_t *filesp, file_offset_t *ffreep, file_offset_t *favailp)
{
	char *e = state->error;

	gfarm_event_free(state->writable);
	gfarm_event_free(state->readable);
	if (e == NULL) {
		*bsizep = state->bsize;
		*blocksp = state->blocks;
		*bfreep = state->bfree;
		*bavailp = state->bavail;
		*filesp = state->files;
		*ffreep = state->ffree;
		*favailp = state->favail;
	}
	free(state);
	return (e);
}

/*
 **********************************************************************
 * Implementation of old (inner gfsd) replication protocol
 **********************************************************************
 */

/* We keep this protocol for protocol compatibility and bootstrapping */
char *
gfs_client_copyin(struct gfs_connection *gfs_server, int src_fd, int fd,
	long sync_rate)
{
	int i, rv, eof;
	long written;
	char *e, *e_tmp;
	char buffer[GFS_PROTO_MAX_IOSIZE];
#ifdef XXX_MEASURE_CKSUM_COST
	/* XXX: should be passed from caller */
	EVP_MD_CTX md_ctx;
	unsigned int md_len;
	unsigned char md_value[EVP_MAX_MD_SIZE];
	const EVP_MD *md_type = EVP_md5(); /* XXX GFS_DEFAULT_DIGEST_MODE; */

	EVP_DigestInit(&md_ctx, md_type);
#endif

	gfs_client_connection_used(gfs_server);

	e = xxx_proto_send(gfs_server->conn, "ii", GFS_PROTO_BULKREAD, src_fd);
	if (e != NULL)
		return (e);
	written = 0;
	for (;;) {
#ifdef XXX_SLOW
		size_t size;

		e = xxx_proto_recv(gfs_server->conn, 0, &eof, "b",
				   sizeof(buffer), &size, buffer);
		if (e != NULL)
			break;
		if (eof) {
			e = GFARM_ERR_PROTOCOL;
			break;
		}
		if (size <= 0)
			break;
#ifdef XXX_MEASURE_CKSUM_COST
		EVP_DigestUpdate(&md_ctx, buffer, size);
#endif
		for (i = 0; i < size; i += rv) {
			rv = write(fd, buffer + i, size - i);
			if (rv <= 0)
				break;
			if (sync_rate != 0) {
				written += rv;
				if (written >= sync_rate) {
					written -= sync_rate;
#ifdef HAVE_FDATASYNC
					fdatasync(fd);
#else
					fsync(fd);
#endif
				}
			}
		}
		if (i < size) {
			/*
			 * write(2) never returns 0,
			 * so the following rv == 0 case is just warm fuzzy.
			 */
			e = gfarm_errno_to_error(rv == 0 ? ENOSPC : errno);
			break;
			/*
			 * XXX - we should return rest of data,
			 * even if write(2) fails.
			 */
		}
#else
		gfarm_int32_t size;

		/* XXX - FIXME layering violation */
		e = xxx_proto_recv(gfs_server->conn, 0, &eof, "i", &size);
		if (e != NULL)
			break;
		if (eof) {
			e = GFARM_ERR_PROTOCOL;
			break;
		}
		if (size <= 0)
			break;
		do {
			/* XXX - FIXME layering violation */
			int partial = xxx_recv_partial(gfs_server->conn, 0,
				buffer, size);

			if (partial <= 0)
				return (GFARM_ERR_PROTOCOL);
#ifdef XXX_MEASURE_CKSUM_COST
			EVP_DigestUpdate(&md_ctx, buffer, partial);
#endif
			size -= partial;
#ifdef __GNUC__ /* shut up stupid warning by gcc */
			rv = 0;
#endif
			for (i = 0; i < partial; i += rv) {
				rv = write(fd, buffer + i, partial - i);
				if (rv <= 0)
					break;
				if (sync_rate != 0) {
					written += rv;
					if (written >= sync_rate) {
						written -= sync_rate;
#ifdef HAVE_FDATASYNC
					fdatasync(fd);
#else
					fsync(fd);
#endif
					}
				}
			}
			if (i < partial) {
				/*
				 * write(2) never returns 0,
				 * so the following rv == 0 case is
				 * just warm fuzzy.
				 */
				return (gfarm_errno_to_error(
						rv == 0 ? ENOSPC : errno));
				/*
				 * XXX - we should return rest of data,
				 * even if write(2) fails.
				 */
			}
		} while (size > 0);
#endif
	}
#ifdef XXX_MEASURE_CKSUM_COST
	/* should be returned to caller */
	EVP_DigestFinal(&md_ctx, md_value, &md_len);
#endif
	e_tmp = gfs_client_rpc_result(gfs_server, 0, "");
	return (e != NULL ? e : e_tmp);
}

/* We keep this protocol for protocol compatibility */

struct gfs_client_striping_copyin_context {
	int ofd;
	file_offset_t offset;
	file_offset_t size;
	int interleave_factor;
	file_offset_t full_stripe_size;

	file_offset_t chunk_residual;
	int io_residual;
	int flags;
#define GCSCC_FINISHED	1
};

/* XXX CKSUM */
char *
gfs_client_striping_copyin_request(struct gfs_connection *gfs_server,
	int src_fd,
	int ofd,
	file_offset_t offset,
	file_offset_t size,
	int interleave_factor,
	file_offset_t full_stripe_size)
{
	char *e = gfs_client_rpc_request(gfs_server,
	    GFS_PROTO_STRIPING_READ, "iooio", src_fd,
	    offset, size, interleave_factor, full_stripe_size);
	struct gfs_client_striping_copyin_context *cc;

	gfs_client_connection_used(gfs_server);

	if (e != NULL)
		return (e);
	e = xxx_proto_flush(gfs_server->conn);
	if (e != NULL)
		return (e);
	GFARM_MALLOC(cc);
	gfs_server->context = cc;
	assert(cc != NULL);

	cc->ofd = ofd;
	cc->offset = offset;
	cc->size = size;
	cc->interleave_factor = interleave_factor;
	cc->full_stripe_size = full_stripe_size;

	cc->chunk_residual = cc->interleave_factor == 0 ? cc->size :
	    cc->interleave_factor;
	cc->io_residual = 0;
	cc->flags = 0;

	return (NULL);
}

char *
gfs_client_striping_copyin_partial(struct gfs_connection *gfs_server, int *rvp)
{
	struct gfs_client_striping_copyin_context *cc = gfs_server->context;
	char *e;
	int eof, partial, i, rv;
	gfarm_int32_t size;
	char buffer[GFS_PROTO_MAX_IOSIZE];

	gfs_client_connection_used(gfs_server);

	if (cc->flags & GCSCC_FINISHED) {
		*rvp = 0;
		return (NULL);
	}
	if (cc->chunk_residual == 0) {
		/* assert(cc->interleave_factor != 0); */
		cc->chunk_residual = cc->interleave_factor;
		cc->io_residual = 0;
		cc->offset += cc->full_stripe_size - cc->interleave_factor;
	}
	if (cc->io_residual == 0) {
		/*
		 * Because we use select(2)/poll(2) as a subsequent i/o
		 * of this statement, we must read just one int32 data only.
		 * Otherwise the select(2)/poll(2) cannot see that
		 * this stream is readable.
		 */
		/* XXX - FIXME layering violation */
		e = xxx_proto_recv(gfs_server->conn, 1, &eof, "i", &size);
		if (e != NULL) {
			cc->flags |= GCSCC_FINISHED;
			return (e);
		}
		if (eof) {
			cc->flags |= GCSCC_FINISHED;
			return (GFARM_ERR_PROTOCOL);
		}
		if (size <= 0) {
			cc->flags |= GCSCC_FINISHED;
			*rvp = size;
			return (NULL);
		}
		cc->io_residual = size;
	}
	/*
	 * Because we use select(2)/poll(2) as a subsequent i/o
	 * of this statement, we must read just `size' bytes only.
	 * Otherwise the select(2)/poll(2) cannot see that
	 * this stream is readable.
	 */
	/* XXX - FIXME layering violation */
	partial = xxx_recv_partial(gfs_server->conn, 1, buffer,
	    cc->io_residual);

	if (partial == 0)
		return (GFARM_ERR_PROTOCOL);
#ifdef XXX_MEASURE_CKSUM_COST
	/* XXX - we cannot calculate checksum on parallel case. */
#endif
	/* We assume that each block never goes across any chunk. */
	cc->io_residual -= partial;
	cc->chunk_residual -= partial;

#ifndef HAVE_PWRITE
	if (lseek(cc->ofd, (off_t)cc->offset, SEEK_SET) == -1)
		return (gfarm_errno_to_error(errno));
#endif
#ifdef __GNUC__ /* shut up stupid warning by gcc */
	rv = 0;
#endif
	for (i = 0; i < partial; i += rv) {
#ifndef HAVE_PWRITE
		rv = write(cc->ofd, buffer + i, partial - i);
#else
		rv = pwrite(cc->ofd, buffer + i, partial - i,
		    (off_t)cc->offset + i);
#endif
		if (rv <= 0)
			break;
	}
	cc->offset += partial;
	if (i < partial) {
		/*
		 * write(2) never returns 0,
		 * so the following rv == 0 case is
		 * just warm fuzzy.
		 */
		return (gfarm_errno_to_error(rv == 0 ? ENOSPC : errno));
		/*
		 * XXX - we should return rest of data,
		 * even if write(2) fails.
		 */
	}
	*rvp = partial;
	return (NULL);
}

char *
gfs_client_striping_copyin_result(struct gfs_connection *gfs_server)
{
	struct gfs_client_striping_copyin_context *cc = gfs_server->context;
	char *e, *e_save = NULL;
	int rv;

	while ((cc->flags & GCSCC_FINISHED) == 0) {
		e = gfs_client_striping_copyin_partial(gfs_server, &rv);
		if (e != NULL && e_save == NULL)
			e_save = e;
	}
	free(gfs_server->context);
	gfs_server->context = NULL;
	e = gfs_client_rpc_result(gfs_server, 0, "");
	return (e != NULL ? e : e_save);
}

char *
gfs_client_striping_copyin(struct gfs_connection *gfs_server,
	int src_fd,
	int ofd,
	file_offset_t offset,
	file_offset_t size,
	int interleave_factor,
	file_offset_t full_stripe_size)
{
	char *e;

	e = gfs_client_striping_copyin_request(gfs_server, src_fd, ofd,
	    offset, size, interleave_factor, full_stripe_size);
	if (e != NULL)
		return (e);
	return (gfs_client_striping_copyin_result(gfs_server));
}

/* We keep this for bootstrapping */
char *
gfs_client_bootstrap_replicate_file_sequential_request(
	struct gfs_connection *gfs_server,
	char *gfarm_file, gfarm_int32_t mode,
	char *src_canonical_hostname, char *src_if_hostname)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc_request(gfs_server,
	    GFS_PROTO_REPLICATE_FILE_SEQUENTIAL, "siss",
	    gfarm_file, mode, src_canonical_hostname, src_if_hostname));
}

/* Perhaps this isn't needed any more, or we can use this for bootstrapping */
char *
gfs_client_bootstrap_replicate_file_parallel_request(
	struct gfs_connection *gfs_server,
	char *gfarm_file, gfarm_int32_t mode, file_offset_t file_size,
	gfarm_int32_t ndivisions, gfarm_int32_t interleave_factor,
	char *src_canonical_hostname, char *src_if_hostname)
{
	gfs_client_connection_used(gfs_server);

	return (gfs_client_rpc_request(gfs_server,
	    GFS_PROTO_REPLICATE_FILE_PARALLEL, "sioiiss",
	    gfarm_file, mode, file_size, ndivisions, interleave_factor,
	    src_canonical_hostname, src_if_hostname));
}

char *
gfs_client_bootstrap_replicate_file_result(struct gfs_connection *gfs_server)
{
	return (gfs_client_rpc_result(gfs_server, 0, ""));
}

char *
gfs_client_bootstrap_replicate_file_request(struct gfs_connection *gfs_server,
	char *gfarm_file, gfarm_int32_t mode, file_offset_t file_size,
	char *src_canonical_hostname, char *src_if_hostname)
{
	char *e;
	struct sockaddr peer_addr;
	long parallel_streams, stripe_unit_size;

	/*
	 * netparam is evaluated here rather than in gfsd,
	 * so, settings in user's .gfarmrc can be reflected.
	 *
	 * XXX - but this also means that settings in frontend host
	 *	is used, rather than settings in the host which does
	 *	actual transfer.
	 */

	e = gfarm_host_address_get(src_if_hostname, gfarm_spool_server_port,
	    &peer_addr, NULL);
	if (e != NULL)
		return (e);

	e = gfarm_netparam_config_get_long(
	    &gfarm_netparam_parallel_streams,
	    src_canonical_hostname, (struct sockaddr *)&peer_addr,
	    &parallel_streams);
	if (e != NULL) /* shouldn't happen */
		return (e);

	if (parallel_streams <= 1)
		return (gfs_client_bootstrap_replicate_file_sequential_request(
		    gfs_server, gfarm_file, mode,
		    src_canonical_hostname, src_if_hostname));

	e = gfarm_netparam_config_get_long(
	    &gfarm_netparam_stripe_unit_size,
	    src_canonical_hostname, (struct sockaddr *)&peer_addr,
	    &stripe_unit_size);
	if (e != NULL) /* shouldn't happen */
		return (e);

	return (
	    gfs_client_bootstrap_replicate_file_parallel_request(gfs_server,
	    gfarm_file, mode, file_size, parallel_streams, stripe_unit_size,
	    src_canonical_hostname, src_if_hostname));
}

char *
gfs_client_bootstrap_replicate_file(struct gfs_connection *gfs_server,
	char *gfarm_file, gfarm_int32_t mode, file_offset_t file_size,
	char *src_canonical_hostname, char *src_if_hostname)
{
	char *e;

	e = gfs_client_bootstrap_replicate_file_request(gfs_server,
	    gfarm_file, mode, file_size,
	    src_canonical_hostname, src_if_hostname);
	if (e == NULL)
		e = gfs_client_bootstrap_replicate_file_result(gfs_server);
	return (e);
}

/*
 **********************************************************************
 * Implementation of gfs_client_command()
 **********************************************************************
 */

struct gfs_client_command_context {
	struct gfarm_iobuffer *iobuffer[NFDESC];

	enum { GFS_COMMAND_SERVER_STATE_NEUTRAL,
		       GFS_COMMAND_SERVER_STATE_OUTPUT,
		       GFS_COMMAND_SERVER_STATE_EXITED,
		       GFS_COMMAND_SERVER_STATE_ABORTED }
		server_state;
	int server_output_fd;
	int server_output_residual;
	enum { GFS_COMMAND_CLIENT_STATE_NEUTRAL,
		       GFS_COMMAND_CLIENT_STATE_OUTPUT }
		client_state;
	int client_output_residual;

	int pid;
	int pending_signal;
};

void
gfs_client_command_set_stdin(struct gfs_connection *gfs_server,
	int (*rf)(struct gfarm_iobuffer *, void *, int, void *, int),
	void *cookie, int fd)
{
	struct gfs_client_command_context *cc = gfs_server->context;

	gfarm_iobuffer_set_read(cc->iobuffer[FDESC_STDIN], rf, cookie, fd);
}

void
gfs_client_command_set_stdout(struct gfs_connection *gfs_server,
	int (*wf)(struct gfarm_iobuffer *, void *, int, void *, int),
	void (*wcf)(struct gfarm_iobuffer *, void *, int),
	void *cookie, int fd)
{
	struct gfs_client_command_context *cc = gfs_server->context;

	gfarm_iobuffer_set_write(cc->iobuffer[FDESC_STDOUT], wf, cookie, fd);
	gfarm_iobuffer_set_write_close(cc->iobuffer[FDESC_STDOUT], wcf);

}

void
gfs_client_command_set_stderr(struct gfs_connection *gfs_server,
	int (*wf)(struct gfarm_iobuffer *, void *, int, void *, int),
	void (*wcf)(struct gfarm_iobuffer *, void *, int),
	void *cookie, int fd)
{
	struct gfs_client_command_context *cc = gfs_server->context;

	gfarm_iobuffer_set_write(cc->iobuffer[FDESC_STDERR], wf, cookie, fd);
	gfarm_iobuffer_set_write_close(cc->iobuffer[FDESC_STDERR], wcf);
}

char *gfs_client_command_request(struct gfs_connection *gfs_server,
				 char *path, char **argv, char **envp,
				 int flags, int *pidp)
{
	struct gfs_client_command_context *cc;
	int na = argv == NULL ? 0 : gfarm_strarray_length(argv);
	int ne = envp == NULL ? 0 : gfarm_strarray_length(envp);
	int conn_fd = xxx_connection_fd(gfs_server->conn);
	int i, xenv_copy, xauth_copy;
	gfarm_int32_t pid;
	socklen_t siz;
	char *e;

	static char *xdisplay_name_cache = NULL;
	static char *xdisplay_env_cache = NULL;
	static int xauth_cached = 0;
	static char *xauth_cache = NULL;
	char *dpy;

	gfs_client_connection_used(gfs_server);

	if ((dpy = getenv("DISPLAY")) != NULL && xdisplay_name_cache == NULL &&
	    (flags & GFS_CLIENT_COMMAND_FLAG_X11MASK) != 0) {
		/*
		 * get $DISPLAY to `xdisplay_name_cache',
		 * and set "DISPLAY=$DISPLAY" to `xdisplay_env_cache'.
		 */
		static char xdisplay_env_format[] = "DISPLAY=%s";
		static char local_prefix[] = "unix:";
		char *prefix;

		if (*dpy == ':') {
			prefix = gfarm_host_get_self_name();
		} else if (memcmp(dpy, local_prefix,
				  sizeof(local_prefix) - 1) == 0) {
			prefix = gfarm_host_get_self_name();
			dpy += sizeof(local_prefix) - 1 - 1;
		} else {
			prefix = "";
		}
		GFARM_MALLOC_ARRAY(xdisplay_name_cache,
			strlen(prefix) + strlen(dpy) + 1);
		if (xdisplay_name_cache == NULL)
			return (GFARM_ERR_NO_MEMORY);
		sprintf(xdisplay_name_cache, "%s%s", prefix, dpy);
		GFARM_MALLOC_ARRAY(xdisplay_env_cache,
		    sizeof(xdisplay_env_format) + strlen(xdisplay_name_cache));
		if (xdisplay_env_cache == NULL) {
			free(xdisplay_name_cache);
			xdisplay_name_cache = NULL;
			return (GFARM_ERR_NO_MEMORY);
		}
		sprintf(xdisplay_env_cache, xdisplay_env_format,
			xdisplay_name_cache);

	}
	if ((flags & GFS_CLIENT_COMMAND_FLAG_X11MASK) ==
	    GFS_CLIENT_COMMAND_FLAG_XAUTHCOPY &&
	    xdisplay_name_cache != NULL && !xauth_cached) {
		/*
		 * get xauth data to `xauth_cache'
		 */
		static char xauth_command_format[] =
			"%s nextract - %s 2>/dev/null";
		char *xauth_command;
		FILE *fp;
		char *s, line[XAUTH_NEXTRACT_MAXLEN];

		GFARM_MALLOC_ARRAY(xauth_command, 
			sizeof(xauth_command_format) +
			strlen(XAUTH_COMMAND) +
			strlen(xdisplay_name_cache));
		if (xauth_command == NULL)
			return (GFARM_ERR_NO_MEMORY);
		sprintf(xauth_command, xauth_command_format,
			XAUTH_COMMAND, xdisplay_name_cache);
		if ((fp = popen(xauth_command, "r")) == NULL) {
			free(xauth_command);
			return (GFARM_ERR_NO_MEMORY);
		}
		s = fgets(line, sizeof line, fp);
		pclose(fp);
		free(xauth_command);
		if (s != NULL) {
			xauth_cache = strdup(line);
			if (xauth_cache == NULL) {
				free(xdisplay_name_cache);
				return (GFARM_ERR_NO_MEMORY);
			}
		}
		xauth_cached = 1;
	}

	xenv_copy =
		(flags & GFS_CLIENT_COMMAND_FLAG_X11MASK) != 0 &&
		xdisplay_env_cache != NULL;
	xauth_copy =
		(flags & GFS_CLIENT_COMMAND_FLAG_X11MASK) ==
		GFS_CLIENT_COMMAND_FLAG_XAUTHCOPY &&
		xauth_cache != NULL;

	/*
	 * don't pass
	 * GFS_CLIENT_COMMAND_FLAG_STDIN_EOF and
	 * GFS_CLIENT_COMMAND_FLAG_XENV_COPY flag via network
	 */
	e = gfs_client_rpc_request(gfs_server,
		GFS_PROTO_COMMAND, "siii",
		path, na,
		ne + (xenv_copy ? 1 : 0),
		((flags & GFS_CLIENT_COMMAND_FLAG_SHELL_COMMAND) ?
		GFS_CLIENT_COMMAND_FLAG_SHELL_COMMAND : 0) |
		(xauth_copy ? GFS_CLIENT_COMMAND_FLAG_XAUTHCOPY : 0));
	if (e != NULL)
		return (e);
	/*
	 * argv
	 */
	for (i = 0; i < na; i++) {
		e = xxx_proto_send(gfs_server->conn, "s", argv[i]);
		if (e != NULL)
			return (e);
	}
	/*
	 * envp
	 */
	for (i = 0; i < ne; i++) {
		e = xxx_proto_send(gfs_server->conn, "s", envp[i]);
		if (e != NULL)
			return (e);
	}
	if (xenv_copy) {
		e = xxx_proto_send(gfs_server->conn, "s", xdisplay_env_cache);
		if (e != NULL)
			return (e);
	}
	if (xauth_copy) {
		/*
		 * xauth
		 */
		e = xxx_proto_send(gfs_server->conn, "s", xauth_cache);
		if (e != NULL)
			return (e);
	}
	/* we have to set `just' flag here */
	e = gfs_client_rpc_result(gfs_server, 1, "i", &pid);
	if (e != NULL)
		return (e);

	gfs_server->context = GFARM_MALLOC(cc);
	if (gfs_server->context == NULL)
		return (GFARM_ERR_NO_MEMORY);

	/*
	 * Now, we set the connection file descriptor non-blocking mode.
	 */
	if (fcntl(conn_fd, F_SETFL, O_NONBLOCK) == -1) {
		free(gfs_server->context);
		gfs_server->context = NULL;
		return (gfarm_errno_to_error(errno));
	}

	/*
	 * initialize gfs_server->context by default values
	 */
	siz = sizeof(i);
	if (getsockopt(conn_fd, SOL_SOCKET, SO_SNDBUF, &i, &siz))
		i = GFARM_DEFAULT_COMMAND_IOBUF_SIZE;
	cc->iobuffer[FDESC_STDIN] = gfarm_iobuffer_alloc(i);

	siz = sizeof(i);
	if (getsockopt(conn_fd, SOL_SOCKET, SO_RCVBUF, &i, &siz))
		i = GFARM_DEFAULT_COMMAND_IOBUF_SIZE;
	cc->iobuffer[FDESC_STDOUT] = gfarm_iobuffer_alloc(i);
	cc->iobuffer[FDESC_STDERR] = gfarm_iobuffer_alloc(i);

	gfarm_iobuffer_set_nonblocking_read_fd(
		cc->iobuffer[FDESC_STDIN], FDESC_STDIN);
	gfarm_iobuffer_set_nonblocking_write_xxx(
		cc->iobuffer[FDESC_STDIN], gfs_server->conn);

	gfarm_iobuffer_set_nonblocking_read_xxx(
		cc->iobuffer[FDESC_STDOUT], gfs_server->conn);
	gfarm_iobuffer_set_nonblocking_write_fd(
		cc->iobuffer[FDESC_STDOUT], FDESC_STDOUT);

	gfarm_iobuffer_set_nonblocking_read_xxx(
		cc->iobuffer[FDESC_STDERR], gfs_server->conn);
	gfarm_iobuffer_set_nonblocking_write_fd(
		cc->iobuffer[FDESC_STDERR], FDESC_STDERR);

	if ((flags & GFS_CLIENT_COMMAND_FLAG_STDIN_EOF) != 0)
		gfarm_iobuffer_set_read_eof(cc->iobuffer[FDESC_STDIN]);

	cc->server_state = GFS_COMMAND_SERVER_STATE_NEUTRAL;
	cc->client_state = GFS_COMMAND_CLIENT_STATE_NEUTRAL;
	cc->pending_signal = 0;

	*pidp = cc->pid = pid;

	return (e);
}

int
gfs_client_command_is_running(struct gfs_connection *gfs_server)
{
	struct gfs_client_command_context *cc = gfs_server->context;

	return (cc->server_state != GFS_COMMAND_SERVER_STATE_EXITED &&
		cc->server_state != GFS_COMMAND_SERVER_STATE_ABORTED);
}

char *
gfs_client_command_fd_set(struct gfs_connection *gfs_server,
			  fd_set *readable, fd_set *writable, int *max_fdp)
{
	struct gfs_client_command_context *cc = gfs_server->context;
	int conn_fd = xxx_connection_fd(gfs_server->conn);
	int i, fd;

	/*
	 * The following test condition should just match with
	 * the i/o condition in gfs_client_command_io_fd_set(),
	 * otherwise unneeded busy wait happens.
	 */

	if (cc->server_state == GFS_COMMAND_SERVER_STATE_NEUTRAL ||
	    (cc->server_state == GFS_COMMAND_SERVER_STATE_OUTPUT &&
	     gfarm_iobuffer_is_readable(cc->iobuffer[cc->server_output_fd]))) {
		FD_SET(conn_fd, readable);
		if (*max_fdp < conn_fd)
			*max_fdp = conn_fd;
	}
	if ((cc->client_state == GFS_COMMAND_CLIENT_STATE_NEUTRAL &&
	     (cc->pending_signal ||
	      gfarm_iobuffer_is_writable(cc->iobuffer[FDESC_STDIN]))) ||
	    cc->client_state == GFS_COMMAND_CLIENT_STATE_OUTPUT) {
		FD_SET(conn_fd, writable);
		if (*max_fdp < conn_fd)
			*max_fdp = conn_fd;
	}

	if (gfarm_iobuffer_is_readable(cc->iobuffer[FDESC_STDIN])) {
		fd = gfarm_iobuffer_get_read_fd(cc->iobuffer[FDESC_STDIN]);
		if (fd < 0) {
			gfarm_iobuffer_read(cc->iobuffer[FDESC_STDIN], NULL);
			/* XXX - if the callback sets an error? */
		} else {
			FD_SET(fd, readable);
			if (*max_fdp < fd)
				*max_fdp = fd;
		}
	}

	for (i = FDESC_STDOUT; i <= FDESC_STDERR; i++) {
		if (gfarm_iobuffer_is_writable(cc->iobuffer[i])) {
			fd = gfarm_iobuffer_get_write_fd(cc->iobuffer[i]);
			if (fd < 0) {
				gfarm_iobuffer_write(cc->iobuffer[i], NULL);
				/* XXX - if the callback sets an error? */
			} else {
				FD_SET(fd, writable);
				if (*max_fdp < fd)
					*max_fdp = fd;
			}
		}
	}
	return (NULL);
}

char *
gfs_client_command_io_fd_set(struct gfs_connection *gfs_server,
			     fd_set *readable, fd_set *writable)
{
	char *e;
	struct gfs_client_command_context *cc = gfs_server->context;
	int i, fd, conn_fd = xxx_connection_fd(gfs_server->conn);

	gfs_client_connection_used(gfs_server);

	fd = gfarm_iobuffer_get_read_fd(cc->iobuffer[FDESC_STDIN]);
	if (fd >= 0 && FD_ISSET(fd, readable)) {
		gfarm_iobuffer_read(cc->iobuffer[FDESC_STDIN], NULL);
		e = gfarm_iobuffer_get_error(cc->iobuffer[FDESC_STDIN]);
		if (e != NULL) {
			/* treat this as eof */
			gfarm_iobuffer_set_read_eof(cc->iobuffer[FDESC_STDIN]);
			/* XXX - how to report this error? */
			gfarm_iobuffer_set_error(cc->iobuffer[FDESC_STDIN],
			    NULL);
		}
	}

	for (i = FDESC_STDOUT; i <= FDESC_STDERR; i++) {
		fd = gfarm_iobuffer_get_write_fd(cc->iobuffer[i]);
		if (fd < 0 || !FD_ISSET(fd, writable))
			continue;
		gfarm_iobuffer_write(cc->iobuffer[i], NULL);
		e = gfarm_iobuffer_get_error(cc->iobuffer[i]);
		if (e == NULL)
			continue;
		/* XXX - just purge the content */
		gfarm_iobuffer_purge(cc->iobuffer[i], NULL);
		/* XXX - how to report this error? */
		gfarm_iobuffer_set_error(cc->iobuffer[i], NULL);
	}

	if (FD_ISSET(conn_fd, readable)) {
		if (cc->server_state == GFS_COMMAND_SERVER_STATE_NEUTRAL) {
			gfarm_int32_t cmd, fd, len;
			int eof;
			char *e;

			e = xxx_proto_recv(gfs_server->conn, 1, &eof,
					   "i", &cmd);
			if (e != NULL || eof) {
				if (e == NULL)
					e = GFARM_ERR_GFSD_ABORTED;
				cc->server_state =
					GFS_COMMAND_SERVER_STATE_ABORTED;
				return (e);
			}
			switch (cmd) {
			case GFS_PROTO_COMMAND_EXITED:
				cc->server_state =
					GFS_COMMAND_SERVER_STATE_EXITED;
				break;
			case GFS_PROTO_COMMAND_FD_OUTPUT:
				e = xxx_proto_recv(gfs_server->conn, 1, &eof,
						   "ii", &fd, &len);
				if (e != NULL || eof) {
					if (e == NULL)
						e = GFARM_ERR_GFSD_ABORTED;
					cc->server_state =
					    GFS_COMMAND_SERVER_STATE_ABORTED;
					return (e);
				}
				if (fd != FDESC_STDOUT && fd != FDESC_STDERR) {
					/* XXX - something wrong */
					cc->server_state =
					    GFS_COMMAND_SERVER_STATE_ABORTED;
					return ("illegal gfsd descriptor");
				}
				if (len <= 0) {
					/* notify closed */
					gfarm_iobuffer_set_read_eof(
					    cc->iobuffer[fd]);
				} else {
					cc->server_state =
					    GFS_COMMAND_SERVER_STATE_OUTPUT;
					cc->server_output_fd = fd;
					cc->server_output_residual = len;
				}
				break;
			default:
				/* XXX - something wrong */
				cc->server_state =
				    GFS_COMMAND_SERVER_STATE_ABORTED;
				return ("unknown gfsd reply");
			}
		} else if (cc->server_state==GFS_COMMAND_SERVER_STATE_OUTPUT) {
			gfarm_iobuffer_read(cc->iobuffer[cc->server_output_fd],
				&cc->server_output_residual);
			if (cc->server_output_residual == 0)
				cc->server_state =
					GFS_COMMAND_SERVER_STATE_NEUTRAL;
			e = gfarm_iobuffer_get_error(
			    cc->iobuffer[cc->server_output_fd]);
			if (e != NULL) {
				/* treat this as eof */
				gfarm_iobuffer_set_read_eof(
				    cc->iobuffer[cc->server_output_fd]);
				gfarm_iobuffer_set_error(
				    cc->iobuffer[cc->server_output_fd], NULL);
				cc->server_state =
					GFS_COMMAND_SERVER_STATE_ABORTED;
				return (e);
			}
			if (gfarm_iobuffer_is_read_eof(
					cc->iobuffer[cc->server_output_fd])) {
				cc->server_state =
					GFS_COMMAND_SERVER_STATE_ABORTED;
				return (GFARM_ERR_GFSD_ABORTED);
			}
		}
	}
	if (FD_ISSET(conn_fd, writable) &&
	    gfs_client_command_is_running(gfs_server)) {
		if (cc->client_state == GFS_COMMAND_CLIENT_STATE_NEUTRAL) {
			if (cc->pending_signal) {
				e = xxx_proto_send(gfs_server->conn, "ii",
					GFS_PROTO_COMMAND_SEND_SIGNAL,
					cc->pending_signal);
				if (e != NULL ||
				    (e = xxx_proto_flush(gfs_server->conn))
				    != NULL) {
					cc->server_state =
					    GFS_COMMAND_SERVER_STATE_ABORTED;
					return (e);
				}
			} else if (gfarm_iobuffer_is_writable(
			    cc->iobuffer[FDESC_STDIN])) {
				/*
				 * cc->client_output_residual may be 0,
				 * if stdin reaches EOF.
				 */
				cc->client_output_residual =
					gfarm_iobuffer_avail_length(
					    cc->iobuffer[FDESC_STDIN]);
				e = xxx_proto_send(gfs_server->conn, "iii",
					GFS_PROTO_COMMAND_FD_INPUT,
					FDESC_STDIN,
					cc->client_output_residual);
				if (e != NULL ||
				    (e = xxx_proto_flush(gfs_server->conn))
				    != NULL) {
					cc->server_state =
					    GFS_COMMAND_SERVER_STATE_ABORTED;
					return (e);
				}
				cc->client_state =
				    GFS_COMMAND_CLIENT_STATE_OUTPUT;
			}
		} else if (cc->client_state==GFS_COMMAND_CLIENT_STATE_OUTPUT) {
			gfarm_iobuffer_write(cc->iobuffer[FDESC_STDIN],
				&cc->client_output_residual);
			if (cc->client_output_residual == 0)
				cc->client_state =
					GFS_COMMAND_CLIENT_STATE_NEUTRAL;
			e = gfarm_iobuffer_get_error(
			    cc->iobuffer[FDESC_STDIN]);
			if (e != NULL) {
				cc->server_state =
					GFS_COMMAND_SERVER_STATE_ABORTED;
				gfarm_iobuffer_set_error(
				    cc->iobuffer[FDESC_STDIN], NULL);
				return (e);
			}
		}
	}
	return (NULL);
}

char *
gfs_client_command_io(struct gfs_connection *gfs_server,
		      struct timeval *timeout)
{
	int nfound, max_fd;
	fd_set readable, writable;
	char *e = NULL;

	if (!gfs_client_command_is_running(gfs_server))
		return (NULL);

	max_fd = -1;
	FD_ZERO(&readable);
	FD_ZERO(&writable);

	gfs_client_command_fd_set(gfs_server, &readable, &writable, &max_fd);
	if (max_fd >= 0) {
		nfound = select(max_fd + 1,
				&readable, &writable, NULL, timeout);
		if (nfound > 0) {
			e = gfs_client_command_io_fd_set(gfs_server,
							 &readable, &writable);
		} else if (nfound == -1 && errno != EINTR) {
			e = gfarm_errno_to_error(errno);
		}
	}

	return (e);
}

int
gfs_client_command_send_stdin(struct gfs_connection *gfs_server,
			      void *data, int len)
{
	struct gfs_client_command_context *cc = gfs_server->context;
	char *p = data;
	int residual = len, rv;

	gfs_client_connection_used(gfs_server);

	while (residual > 0 && gfs_client_command_is_running(gfs_server)) {
		rv = gfarm_iobuffer_put(cc->iobuffer[FDESC_STDIN],
					p, residual);
		p += rv;
		residual -= rv;
		gfs_client_command_io(gfs_server, NULL);
		/* XXX - how to report this error? */
	}
	return (len - residual);
}

void
gfs_client_command_close_stdin(struct gfs_connection *gfs_server)
{
	struct gfs_client_command_context *cc = gfs_server->context;

	gfarm_iobuffer_set_read_eof(cc->iobuffer[FDESC_STDIN]);
}

char *
gfs_client_command_send_signal(struct gfs_connection *gfs_server, int sig)
{
	char *e;
	struct gfs_client_command_context *cc = gfs_server->context;

	gfs_client_connection_used(gfs_server);

	while (gfs_client_command_is_running(gfs_server) &&
	       cc->pending_signal != 0) {
		e = gfs_client_command_io(gfs_server, NULL);
		if (e != NULL)
			return (e);
	}
	if (!gfs_client_command_is_running(gfs_server))
		return (gfarm_errno_to_error(ESRCH));
	cc->pending_signal = sig;
	/* make a chance to send the signal immediately */
	return (gfs_client_command_io(gfs_server, NULL));
}

char *
gfs_client_command_result(struct gfs_connection *gfs_server,
	int *term_signal, int *exit_status, int *exit_flag)
{
	struct gfs_client_command_context *cc = gfs_server->context;
	char *e;

	while (gfs_client_command_is_running(gfs_server)) {
		gfs_client_command_io(gfs_server, NULL);
		/* XXX - how to report this error? */
	}

	/*
	 * flush stdout/stderr
	 */
	while (gfarm_iobuffer_is_writable(cc->iobuffer[FDESC_STDOUT]) ||
	       gfarm_iobuffer_is_writable(cc->iobuffer[FDESC_STDERR])) {
		int i, nfound, fd, max_fd = -1;
		fd_set writable;

		FD_ZERO(&writable);
		for (i = FDESC_STDOUT; i <= FDESC_STDERR; i++) {
			if (gfarm_iobuffer_is_writable(cc->iobuffer[i])) {
				fd = gfarm_iobuffer_get_write_fd(
				    cc->iobuffer[i]);
				if (fd < 0) {
					gfarm_iobuffer_write(cc->iobuffer[i],
					    NULL);
					/*
					 * XXX - if the callback sets an error?
					 */
				} else {
					FD_SET(fd, &writable);
					if (max_fd < fd)
						max_fd = fd;
				}
			}
		}

		if (max_fd < 0)
			continue;

		nfound = select(max_fd + 1, NULL, &writable, NULL, NULL);
		if (nfound == -1 && errno != EINTR)
			break;

		if (nfound > 0) {
			for (i = FDESC_STDOUT; i <= FDESC_STDERR; i++) {
				fd = gfarm_iobuffer_get_write_fd(
				    cc->iobuffer[i]);
				if (fd < 0 || !FD_ISSET(fd, &writable))
					continue;
				gfarm_iobuffer_write(cc->iobuffer[i], NULL);
				e = gfarm_iobuffer_get_error(cc->iobuffer[i]);
				if (e == NULL)
					continue;
				/* XXX - just purge the content */
				gfarm_iobuffer_purge(cc->iobuffer[i], NULL);
				/* XXX - how to report this error? */
				gfarm_iobuffer_set_error(cc->iobuffer[i],NULL);
			}
		}
	}

	/*
	 * context isn't needed anymore
	 */
	gfarm_iobuffer_free(cc->iobuffer[FDESC_STDIN]);
	gfarm_iobuffer_free(cc->iobuffer[FDESC_STDOUT]);
	gfarm_iobuffer_free(cc->iobuffer[FDESC_STDERR]);
	free(gfs_server->context);
	gfs_server->context = NULL;
	/*
	 * Now, we recover the connection file descriptor blocking mode.
	 */
	if (fcntl(xxx_connection_fd(gfs_server->conn), F_SETFL, 0) == -1) {
		return (gfarm_errno_to_error(errno));
	}
	return (gfs_client_rpc(gfs_server, 0, GFS_PROTO_COMMAND_EXIT_STATUS,
			       "/iii",
			       term_signal, exit_status, exit_flag));
}

char *
gfs_client_command(struct gfs_connection *gfs_server,
	char *path, char **argv, char **envp, int flags,
	int *term_signal, int *exit_status, int *exit_flag)
{
	char *e, *e2;
	int pid;

	e = gfs_client_command_request(gfs_server, path, argv, envp, flags,
				       &pid);
	if (e)
		return (e);
	while (gfs_client_command_is_running(gfs_server))
		e = gfs_client_command_io(gfs_server, NULL);
	e2 = gfs_client_command_result(gfs_server,
				       term_signal, exit_status, exit_flag);
	return (e != NULL ? e : e2);
}

/*
 **********************************************************************
 * functions which try to reconnect the server
 * when the cached connection is dead.
 **********************************************************************
 */

char *
gfs_client_connection_acquire_by_host(const char *hostname,
	struct gfs_connection **gfs_serverp)
{
	char *e;
	struct sockaddr peer_addr;

	e = gfarm_host_address_get(hostname, gfarm_spool_server_port,
	    &peer_addr, NULL);
	if (e != NULL)
		return (e);
	return (gfs_client_connection_acquire(hostname, &peer_addr,
	    gfs_serverp));
}

static void
gfs_client_connection_return_or_free(
	struct gfs_connection *gfs_server, char *error,
	struct gfs_connection **gfs_serverp, char **errorp)
{
	if (gfs_serverp != NULL) {
		*gfs_serverp = gfs_server;
	} else {
		gfs_client_connection_free(gfs_server);
		/*
		 * We won't return the error of gfs_client_connection_free().
		 * If the caller want see this error, it should pass
		 * gfs_serverp which is not NULL.
		 */
	}
	if (errorp != NULL)
		*errorp = error;
}

char *
gfs_client_open_with_reconnect_addr(
	const char *hostname, struct sockaddr *peer_addr,
	char *gfarm_file, gfarm_int32_t flag, gfarm_int32_t mode,
	struct gfs_connection **gfs_serverp,
	char **op_errorp, gfarm_int32_t *fdp)
{
	char *e;
	struct gfs_connection *gfs_server;

	e = gfs_client_connection_acquire(hostname, peer_addr, &gfs_server);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	e = gfs_client_open(gfs_server, gfarm_file, flag, mode, fdp);
	if (gfs_client_is_connection_error(e)) {
		gfs_client_connection_free(gfs_server);
		e = gfs_client_connection_acquire(hostname, peer_addr,
		    &gfs_server);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		e = gfs_client_open(gfs_server, gfarm_file, flag, mode, fdp);
	}
	gfs_client_connection_return_or_free(gfs_server, e,
	    gfs_serverp, op_errorp);
	return (NULL);
}

char *
gfs_client_open_with_reconnection(const char *hostname,
	char *gfarm_file, gfarm_int32_t flag, gfarm_int32_t mode,
	struct gfs_connection **gfs_serverp,
	char **op_errorp, gfarm_int32_t *fdp)
{
	char *e;
	struct sockaddr peer_addr;

	e = gfarm_host_address_get(hostname, gfarm_spool_server_port,
	    &peer_addr, NULL);
	if (e != NULL)
		return (e);
	return (gfs_client_open_with_reconnect_addr(hostname, &peer_addr,
	    gfarm_file, flag, mode, gfs_serverp, op_errorp, fdp));
}

char *
gfs_client_open_local_with_reconnection(const char *hostname,
	char *gfarm_file, gfarm_int32_t flag, gfarm_int32_t mode,
	struct gfs_connection **gfs_serverp,
	char **op_errorp, gfarm_int32_t *fdp)
{
	char *e;
	struct gfs_connection *gfs_server;

	e = gfs_client_connection_acquire_by_host(hostname, &gfs_server);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	e = gfs_client_open_local(gfs_server, gfarm_file, flag, mode, fdp);
	if (gfs_client_is_connection_error(e)) {
		gfs_client_connection_free(gfs_server);
		e = gfs_client_connection_acquire_by_host(hostname,
		    &gfs_server);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		e = gfs_client_open_local(gfs_server, gfarm_file, flag, mode,
		    fdp);
	}
	gfs_client_connection_return_or_free(gfs_server, e,
	    gfs_serverp, op_errorp);
	return (NULL);
}

char *
gfs_client_link_with_reconnection(const char *hostname, char *from, char *to,
	struct gfs_connection **gfs_serverp, char **op_errorp)
{
	char *e;
	struct gfs_connection *gfs_server;

	e = gfs_client_connection_acquire_by_host(hostname, &gfs_server);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	e = gfs_client_link(gfs_server, from, to);
	if (gfs_client_is_connection_error(e)) {
		gfs_client_connection_free(gfs_server);
		e = gfs_client_connection_acquire_by_host(hostname,
		    &gfs_server);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		e = gfs_client_link(gfs_server, from, to);
	}
	gfs_client_connection_return_or_free(gfs_server, e,
	    gfs_serverp, op_errorp);
	return (NULL);
}

char *
gfs_client_unlink_with_reconnection(const char *hostname, char *path,
	struct gfs_connection **gfs_serverp, char **op_errorp)
{
	char *e;
	struct gfs_connection *gfs_server;

	e = gfs_client_connection_acquire_by_host(hostname, &gfs_server);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	e = gfs_client_unlink(gfs_server, path);
	if (gfs_client_is_connection_error(e)) {
		gfs_client_connection_free(gfs_server);
		e = gfs_client_connection_acquire_by_host(hostname,
		    &gfs_server);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		e = gfs_client_unlink(gfs_server, path);
	}
	gfs_client_connection_return_or_free(gfs_server, e,
	    gfs_serverp, op_errorp);
	return (NULL);
}

char *
gfs_client_rename_with_reconnection(const char *hostname, char *from, char *to,
	struct gfs_connection **gfs_serverp, char **op_errorp)
{
	char *e;
	struct gfs_connection *gfs_server;

	e = gfs_client_connection_acquire_by_host(hostname, &gfs_server);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	e = gfs_client_rename(gfs_server, from, to);
	if (gfs_client_is_connection_error(e)) {
		gfs_client_connection_free(gfs_server);
		e = gfs_client_connection_acquire_by_host(hostname,
		    &gfs_server);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		e = gfs_client_rename(gfs_server, from, to);
	}
	gfs_client_connection_return_or_free(gfs_server, e,
	    gfs_serverp, op_errorp);
	return (NULL);
}

char *
gfs_client_chmod_with_reconnection(const char *hostname,
	char *path, gfarm_int32_t mode,
	struct gfs_connection **gfs_serverp, char **op_errorp)
{
	char *e;
	struct gfs_connection *gfs_server;

	e = gfs_client_connection_acquire_by_host(hostname, &gfs_server);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	e = gfs_client_chmod(gfs_server, path, mode);
	if (gfs_client_is_connection_error(e)) {
		gfs_client_connection_free(gfs_server);
		e = gfs_client_connection_acquire_by_host(hostname,
		    &gfs_server);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		e = gfs_client_chmod(gfs_server, path, mode);
	}
	gfs_client_connection_return_or_free(gfs_server, e,
	    gfs_serverp, op_errorp);
	return (NULL);
}

char *
gfs_client_get_spool_root_with_reconnection(const char *hostname,
	struct gfs_connection **gfs_serverp,
	char **op_errorp, char **spool_rootp)
{
	char *e;
	struct gfs_connection *gfs_server;

	e = gfs_client_connection_acquire_by_host(hostname, &gfs_server);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	e = gfs_client_get_spool_root(gfs_server, spool_rootp);
	if (gfs_client_is_connection_error(e)) {
		gfs_client_connection_free(gfs_server);
		e = gfs_client_connection_acquire_by_host(hostname,
		    &gfs_server);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		e = gfs_client_get_spool_root(gfs_server, spool_rootp);
	}
	gfs_client_connection_return_or_free(gfs_server, e,
	    gfs_serverp, op_errorp);
	return (NULL);
}

char *
gfs_client_statfs_with_reconnect_addr(
	const char *hostname, struct sockaddr *peer_addr, char *path,
	struct gfs_connection **gfs_serverp, char **op_errorp,
	gfarm_int32_t *bsizep,
	file_offset_t *blocksp, file_offset_t *bfreep, file_offset_t *bavailp,
	file_offset_t *filesp, file_offset_t *ffreep, file_offset_t *favailp)
{
	char *e;
	struct gfs_connection *gfs_server;

	e = gfs_client_connection_acquire(hostname, peer_addr, &gfs_server);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	e = gfs_client_statfs(gfs_server, path,
	    bsizep, blocksp, bfreep, bavailp, filesp, ffreep, favailp);
	if (gfs_client_is_connection_error(e)) {
		gfs_client_connection_free(gfs_server);
		e = gfs_client_connection_acquire(hostname, peer_addr,
		    &gfs_server);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		e = gfs_client_statfs(gfs_server, path,
		    bsizep, blocksp, bfreep, bavailp, filesp, ffreep, favailp);
	}
	gfs_client_connection_return_or_free(gfs_server, e,
	    gfs_serverp, op_errorp);
	return (NULL);
}

char *
gfs_client_bootstrap_replicate_file_with_reconnection(
	char *dst_canonical_hostname,
	char *gfarm_file, gfarm_int32_t mode, file_offset_t file_size,
	char *src_canonical_hostname, char *src_if_hostname,
	struct gfs_connection **dst_gfs_serverp, char **op_errorp)
{
	char *e;
	struct gfs_connection *dst_gfs_server;

	e = gfs_client_connection_acquire_by_host(dst_canonical_hostname,
	    &dst_gfs_server);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	e = gfs_client_bootstrap_replicate_file(dst_gfs_server,
	    gfarm_file, mode, file_size,
	    src_canonical_hostname, src_if_hostname);
	if (gfs_client_is_connection_error(e)) {
		gfs_client_connection_free(dst_gfs_server);
		e = gfs_client_connection_acquire_by_host(
		    dst_canonical_hostname, &dst_gfs_server);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		e = gfs_client_bootstrap_replicate_file(dst_gfs_server,
		    gfarm_file, mode, file_size,
		    src_canonical_hostname, src_if_hostname);
	}
	gfs_client_connection_return_or_free(dst_gfs_server, e,
	    dst_gfs_serverp, op_errorp);
	return (NULL);
}

/*
 **********************************************************************
 * gfsd datagram service
 **********************************************************************
 */

int gfs_client_datagram_timeouts[] = { /* milli seconds */
	8000, 12000, 18000, 27000
};
int gfs_client_datagram_ntimeouts =
	GFARM_ARRAY_LENGTH(gfs_client_datagram_timeouts);

/*
 * `server_addr_size' should be socklen_t, but that requires <sys/socket.h>
 * for all source files which include "gfs_client.h".
 */
char *
gfs_client_get_load_request(int sock,
	struct sockaddr *server_addr, int server_addr_size)
{
	int rv, command = 0;

	if (server_addr == NULL || server_addr_size == 0) {
		/* using connected UDP socket */
		rv = write(sock, &command, sizeof(command));
	} else {
		rv = sendto(sock, &command, sizeof(command), 0,
		    server_addr, server_addr_size);
	}
	if (rv == -1)
		return (gfarm_errno_to_error(errno));
	return (NULL);
}

/*
 * `*server_addr_sizep' is an IN/OUT parameter.
 *
 * `*server_addr_sizep' should be socklen_t, but that requires <sys/socket.h>
 * for all source files which include "gfs_client.h".
 */
char *
gfs_client_get_load_result(int sock,
	struct sockaddr *server_addr, socklen_t *server_addr_sizep,
	struct gfs_client_load *result)
{
	int rv;
	double loadavg[3];
#ifndef WORDS_BIGENDIAN
	struct { char c[8]; } nloadavg[3];
#else
#	define nloadavg loadavg
#endif

	if (server_addr == NULL || server_addr_sizep == NULL) {
		/* caller doesn't need server_addr */
		rv = read(sock, nloadavg, sizeof(nloadavg));
	} else {
		rv = recvfrom(sock, nloadavg, sizeof(nloadavg), 0,
		    server_addr, server_addr_sizep);
	}
	if (rv == -1)
		return (gfarm_errno_to_error(errno));

#ifndef WORDS_BIGENDIAN /* prototype of swab() uses (char *) on Solaris */
	swab((void *)&nloadavg[0], (void *)&loadavg[0], sizeof(loadavg[0]));
	swab((void *)&nloadavg[1], (void *)&loadavg[1], sizeof(loadavg[1]));
	swab((void *)&nloadavg[2], (void *)&loadavg[2], sizeof(loadavg[2]));
#endif
	result->loadavg_1min = loadavg[0];
	result->loadavg_5min = loadavg[1];
	result->loadavg_15min = loadavg[2];
	return (NULL);
}

/*
 * multiplexed version of gfs_client_get_load()
 */

struct gfs_client_get_load_state {
	struct gfarm_eventqueue *q;
	struct gfarm_event *writable, *readable;
	void (*continuation)(void *);
	void *closure;

	int sock;
	int try;

	/* results */
	char *error;
	struct gfs_client_load load;
};

static void
gfs_client_get_load_send(int events, int fd, void *closure,
	const struct timeval *t)
{
	struct gfs_client_get_load_state *state = closure;
	int rv;
	struct timeval timeout;

	state->error = gfs_client_get_load_request(state->sock, NULL, 0);
	if (state->error == NULL) {
		timeout.tv_sec = timeout.tv_usec = 0;
		gfarm_timeval_add_microsec(&timeout,
		    gfs_client_datagram_timeouts[state->try] *
		    GFARM_MILLISEC_BY_MICROSEC);
		if ((rv = gfarm_eventqueue_add_event(state->q, state->readable,
		    &timeout)) == 0) {
			/* go to gfs_client_get_load_receive() */
			return;
		}
		state->error = gfarm_errno_to_error(rv);
	}
	close(state->sock);
	state->sock = -1;
	if (state->continuation != NULL)
		(*state->continuation)(state->closure);
}

static void
gfs_client_get_load_receive(int events, int fd, void *closure,
	const struct timeval *t)
{
	struct gfs_client_get_load_state *state = closure;
	int rv;

	if ((events & GFARM_EVENT_TIMEOUT) != 0) {
		assert(events == GFARM_EVENT_TIMEOUT);
		++state->try;
		if (state->try >= gfs_client_datagram_ntimeouts) {
			state->error = GFARM_ERR_CONNECTION_TIMED_OUT;
		} else if ((rv = gfarm_eventqueue_add_event(state->q,
		    state->writable, NULL)) == 0) {
			/* go to gfs_client_get_load_send() */
			return;
		} else {
			state->error = gfarm_errno_to_error(rv);
		}
	} else {
		assert(events == GFARM_EVENT_READ);
		state->error = gfs_client_get_load_result(state->sock,
		    NULL, NULL, &state->load);
	}
	close(state->sock);
	state->sock = -1;
	if (state->continuation != NULL)
		(*state->continuation)(state->closure);
}

char *
gfs_client_get_load_request_multiplexed(struct gfarm_eventqueue *q,
	struct sockaddr *peer_addr,
	void (*continuation)(void *), void *closure,
	struct gfs_client_get_load_state **statepp)
{
	char *e;
	int rv, sock;
	struct gfs_client_get_load_state *state;

	/* use different socket for each peer, to identify error code */
	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock == -1) {
		e = gfarm_errno_to_error(errno);
		goto error_return;
	}
	fcntl(sock, F_SETFD, 1); /* automatically close() on exec(2) */
	/* connect UDP socket, to get error code */
	if (connect(sock, peer_addr, sizeof(*peer_addr)) == -1) {
		e = gfarm_errno_to_error(errno);
		goto error_close_sock;
	}

	GFARM_MALLOC(state);
	if (state == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		goto error_close_sock;
	}

	state->writable = gfarm_fd_event_alloc(
	    GFARM_EVENT_WRITE, sock,
	    gfs_client_get_load_send, state);
	if (state->writable == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		goto error_free_state;
	}
	/*
	 * We cannot use two independent events (i.e. a fd_event with
	 * GFARM_EVENT_READ flag and a timer_event) here, because
	 * it's possible that both event handlers are called at once.
	 */
	state->readable = gfarm_fd_event_alloc(
	    GFARM_EVENT_READ|GFARM_EVENT_TIMEOUT, sock,
	    gfs_client_get_load_receive, state);
	if (state->readable == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		goto error_free_writable;
	}
	/* go to gfs_client_get_load_send() */
	rv = gfarm_eventqueue_add_event(q, state->writable, NULL);
	if (rv != 0) {
		e = gfarm_errno_to_error(rv);
		goto error_free_readable;
	}

	state->q = q;
	state->continuation = continuation;
	state->closure = closure;
	state->sock = sock;
	state->try = 0;
	state->error = NULL;
	*statepp = state;
	return (NULL);

error_free_readable:
	gfarm_event_free(state->readable);
error_free_writable:
	gfarm_event_free(state->writable);
error_free_state:
	free(state);
error_close_sock:
	close(sock);
error_return:
	return (e);
}

char *
gfs_client_get_load_result_multiplexed(
	struct gfs_client_get_load_state *state, struct gfs_client_load *loadp)
{
	char *error = state->error;

	if (state->sock >= 0) { /* sanity */
		close(state->sock);
		state->sock = -1;
	}
	if (error == NULL)
		*loadp = state->load;
	gfarm_event_free(state->readable);
	gfarm_event_free(state->writable);
	free(state);
	return (error);
}
