/*
 * $Id: agent_ptable.h 1728 2005-04-12 15:28:23Z tatebe $
 */

void agent_ptable_alloc(void);
void *agent_ptable_entry_get(int);
int agent_ptable_entry_add(void *);
int agent_ptable_entry_delete(int);
