/*
 * $Id: gfarm_acl.h 7761 2013-02-14 12:01:22Z takuya-i $
 */

void gfvfs_acl_id_init();
SMB_ACL_T gfvfs_gfarm_acl_to_smb_acl(const char *, gfarm_acl_t);
gfarm_acl_t gfvfs_smb_acl_to_gfarm_acl(const char *, SMB_ACL_T);
