/*
 * $Id: replica_check.h 10647 2017-10-30 03:29:27Z takuya-i $
 */

void replica_check_init(void);

void replica_check_start_host_up(void);
void replica_check_start_host_down(void);
void replica_check_start_xattr_update(void);
void replica_check_start_move(void);
void replica_check_start_rep_request_failed(void);
void replica_check_start_rep_result_failed(void);
void replica_check_start_fsngroup_modify(void);
void replica_check_start_host_is_not_busy(void);

void replica_check_info(void);

gfarm_error_t gfm_server_replica_check_ctrl(struct peer *, int, int);
gfarm_error_t gfm_server_replica_check_status(struct peer *, int, int);
