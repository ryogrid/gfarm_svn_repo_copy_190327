/*
 * $Id: sockopt.h 346 2003-04-16 10:58:20Z soda $
 */

struct gfarm_hostspec;

char *gfarm_sockopt_config_add(char *, struct gfarm_hostspec *);
char *gfarm_sockopt_apply_by_name_addr(int, const char *, struct sockaddr *);
char *gfarm_sockopt_listener_config_add(char *);
char *gfarm_sockopt_apply_listener(int);
