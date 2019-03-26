/*
 * $Id: agent_proto.h 3725 2007-05-11 12:46:50Z tatebe $
 */

enum gfarm_agent_proto_command {
	AGENT_PROTO_PATH_INFO_GET,
	AGENT_PROTO_PATH_INFO_SET,
	AGENT_PROTO_PATH_INFO_REPLACE,
	AGENT_PROTO_PATH_INFO_REMOVE,
	AGENT_PROTO_REALPATH_CANONICAL,
	AGENT_PROTO_GET_INO,
	AGENT_PROTO_OPENDIR,
	AGENT_PROTO_READDIR,
	AGENT_PROTO_CLOSEDIR,
	AGENT_PROTO_DIRNAME,
	AGENT_PROTO_UNCACHEDIR,
	AGENT_PROTO_HOST_INFO_GET,
	AGENT_PROTO_HOST_INFO_REMOVE_HOSTALIASES,
	AGENT_PROTO_HOST_INFO_SET,
	AGENT_PROTO_HOST_INFO_REPLACE,
	AGENT_PROTO_HOST_INFO_REMOVE,
	AGENT_PROTO_HOST_INFO_GET_ALL,
	AGENT_PROTO_HOST_INFO_GET_BY_NAME_ALIAS,
	AGENT_PROTO_HOST_INFO_GET_ALLHOST_BY_ARCHITECTURE,
	AGENT_PROTO_FILE_SECTION_INFO_GET,
	AGENT_PROTO_FILE_SECTION_INFO_SET,
	AGENT_PROTO_FILE_SECTION_INFO_REPLACE,
	AGENT_PROTO_FILE_SECTION_INFO_REMOVE,
	AGENT_PROTO_FILE_SECTION_INFO_GET_ALL_BY_FILE,
	AGENT_PROTO_FILE_SECTION_COPY_INFO_GET,
	AGENT_PROTO_FILE_SECTION_COPY_INFO_SET,
	AGENT_PROTO_FILE_SECTION_COPY_INFO_REMOVE,
	AGENT_PROTO_FILE_SECTION_COPY_INFO_GET_ALL_BY_FILE,
	AGENT_PROTO_FILE_SECTION_COPY_INFO_GET_ALL_BY_SECTION,
	AGENT_PROTO_FILE_SECTION_COPY_INFO_GET_ALL_BY_HOST,
	AGENT_PROTO_SEEKDIR,
	AGENT_PROTO_TELLDIR,
	AGENT_PROTO_DIR_GET_NENTRY, /* reservation */
	AGENT_PROTO_PATH_INFO_XATTR_GET,
	AGENT_PROTO_PATH_INFO_XATTR_SET,
	AGENT_PROTO_PATH_INFO_XATTR_REPLACE,
	AGENT_PROTO_PATH_INFO_XATTR_REMOVE
};

char *xxx_proto_send_host_info(
	struct xxx_connection *, const struct gfarm_host_info *);
char *xxx_proto_recv_host_info(
	struct xxx_connection *, struct gfarm_host_info *);
char *xxx_proto_send_host_info_for_set(
	struct xxx_connection *, const struct gfarm_host_info *);
char *xxx_proto_recv_host_info_for_set(
	struct xxx_connection *, struct gfarm_host_info *);
char *xxx_proto_send_path_info(
	struct xxx_connection *, const struct gfarm_path_info *);
char *xxx_proto_recv_path_info(
	struct xxx_connection *, struct gfarm_path_info *);
char *xxx_proto_send_path_info_for_set(
	struct xxx_connection *, const struct gfarm_path_info *);
char *xxx_proto_recv_path_info_for_set(
	struct xxx_connection *, struct gfarm_path_info *);
char *xxx_proto_send_file_section_info(
	struct xxx_connection *, const struct gfarm_file_section_info *);
char *xxx_proto_recv_file_section_info(
	struct xxx_connection *, struct gfarm_file_section_info *);
char *xxx_proto_send_file_section_info_for_set(
	struct xxx_connection *, const struct gfarm_file_section_info *);
char *xxx_proto_recv_file_section_info_for_set(
	struct xxx_connection *, struct gfarm_file_section_info *);
char *xxx_proto_send_file_section_copy_info(
	struct xxx_connection *, const struct gfarm_file_section_copy_info *);
char *xxx_proto_recv_file_section_copy_info(
	struct xxx_connection *, struct gfarm_file_section_copy_info *);
