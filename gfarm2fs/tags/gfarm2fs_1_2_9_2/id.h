/*
 * $Id: id.h 7091 2012-11-16 04:26:58Z takuya-i $
 */

void gfarm2fs_id_init(struct gfarm2fs_param *);

gfarm_error_t gfarm2fs_get_uid(const char *, const char *, uid_t *);
gfarm_error_t gfarm2fs_get_gid(const char *, const char *, gid_t *);
gfarm_error_t gfarm2fs_get_user(const char *, uid_t, char **);
gfarm_error_t gfarm2fs_get_group(const char *, gid_t, char **);

uid_t gfarm2fs_get_nobody_uid(void);
gid_t gfarm2fs_get_nogroup_gid(void);
