/*
 * $Id: gfarm_cmd.h 9753 2015-06-24 00:13:12Z takuya-i $
 */

pid_t gfarm_popen3(char *const [], int *, int *, int *);
int gfarm_cmd_exec(char *const [], int (*)(int, void *), void *, int, int);
