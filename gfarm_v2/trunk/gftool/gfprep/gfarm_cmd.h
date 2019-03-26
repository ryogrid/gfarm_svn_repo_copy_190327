/*
 * $Id: gfarm_cmd.h 9756 2015-06-24 01:44:35Z takuya-i $
 */

pid_t gfarm_popen3(char *const [], int *, int *, int *);
int gfarm_cmd_exec(char *const [], int (*)(int, void *), void *, int, int);
