/*
 * $Id: agent_thr.h 1753 2005-04-19 08:53:39Z tatebe $
 */

void agent_lock(void);
void agent_unlock(void);

int agent_schedule(void *, void *(*)(void *));
