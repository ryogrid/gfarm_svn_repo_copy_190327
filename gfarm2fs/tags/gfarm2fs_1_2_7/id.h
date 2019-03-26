/*
 * $Id: id.h 5069 2011-01-13 10:40:16Z takuya-i $
 */

void gfarm2fs_id_init(struct gfarm2fs_param *);

gfarm_error_t gfarm2fs_get_uid(const char *, const char *, uid_t *);
gfarm_error_t gfarm2fs_get_gid(const char *, const char *, gid_t *);
gfarm_error_t gfarm2fs_get_user(const char *, uid_t, char **);
gfarm_error_t gfarm2fs_get_group(const char *, gid_t, char **);

uid_t gfarm2fs_get_nobody_uid();
gid_t gfarm2fs_get_nogroup_gid();
