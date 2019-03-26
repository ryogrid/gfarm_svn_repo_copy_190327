/*
 * $Id: bool.h 7393 2012-12-06 07:14:04Z takuya-i $
 */
#if !defined(__cplusplus)
#if __STDC_VERSION__ >= 199901L
#include <stdbool.h>
#else
typedef enum {
	false = 0,
	true = 1
} bool;
#endif /* __STDC_VERSION__ >= 199901L */
#endif /* ! __cplusplus */
