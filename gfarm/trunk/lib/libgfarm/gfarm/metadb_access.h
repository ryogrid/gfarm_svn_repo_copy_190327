/*
 * Metadata server operations
 *
 * $Id: metadb_access.h 3725 2007-05-11 12:46:50Z tatebe $
 */

char *gfarm_metadb_initialize(void);
char *gfarm_metadb_terminate(void);
void gfarm_metadb_share_connection(void);

char *gfarm_metadb_host_info_get(const char *, struct gfarm_host_info *);
char *gfarm_metadb_host_info_remove_hostaliases(const char *);
char *gfarm_metadb_host_info_set(const char *, const struct gfarm_host_info *);
char *gfarm_metadb_host_info_replace(const char *,
	const struct gfarm_host_info *);
char *gfarm_metadb_host_info_remove(const char *);
char *gfarm_metadb_host_info_get_all(int *, struct gfarm_host_info **);
char *gfarm_metadb_host_info_get_by_name_alias(const char *,
	struct gfarm_host_info *);
char *gfarm_metadb_host_info_get_allhost_by_architecture(const char *,
	int *, struct gfarm_host_info **);

char *gfarm_metadb_path_info_get(const char *, struct gfarm_path_info *);
char *gfarm_metadb_path_info_set(const char *, const struct gfarm_path_info *);
char *gfarm_metadb_path_info_replace(const char *,
	const struct gfarm_path_info *);
char *gfarm_metadb_path_info_remove(const char *);
char *gfarm_metadb_path_info_get_all_foreach(
	void (*)(void *, struct gfarm_path_info *), void *);

char *gfarm_metadb_path_info_xattr_get(
	const char *, struct gfarm_path_info_xattr *);
char *gfarm_metadb_path_info_xattr_set(const struct gfarm_path_info_xattr *);
char *gfarm_metadb_path_info_xattr_replace(
	const struct gfarm_path_info_xattr *);
char *gfarm_metadb_path_info_xattr_remove(const char *);

char *gfarm_metadb_file_section_info_get(
	const char *, const char *, struct gfarm_file_section_info *);
char *gfarm_metadb_file_section_info_set(
	const char *, const char *, const struct gfarm_file_section_info *);
char *gfarm_metadb_file_section_info_replace(
	const char *, const char *, const struct gfarm_file_section_info *);
char *gfarm_metadb_file_section_info_remove(const char *, const char *);
char *gfarm_metadb_file_section_info_get_all_by_file(
	const char *, int *, struct gfarm_file_section_info **);

char *gfarm_metadb_file_section_copy_info_get(
	const char *, const char *, const char *,
	struct gfarm_file_section_copy_info *);
char *gfarm_metadb_file_section_copy_info_set(
	const char *, const char *, const char *,
	const struct gfarm_file_section_copy_info *);
char *gfarm_metadb_file_section_copy_info_remove(
	const char *, const char *, const char *);
char *gfarm_metadb_file_section_copy_info_get_all_by_file(const char *, int *,
	struct gfarm_file_section_copy_info **);
char *gfarm_metadb_file_section_copy_info_get_all_by_section(const char *,
	const char *, int *, struct gfarm_file_section_copy_info **);
char *gfarm_metadb_file_section_copy_info_get_all_by_host(
	const char *, int *, struct gfarm_file_section_copy_info **);

/* external interface to select metadb backend type */
char *gfarm_metadb_use_none(void);
char *gfarm_metadb_use_ldap(void);
char *gfarm_metadb_use_postgresql(void);
char *gfarm_metadb_use_localfs(void);
