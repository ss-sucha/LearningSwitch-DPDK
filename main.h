#ifndef _MAIN_H_
#define _MAIN_H_

#include <sys/queue.h>


/* Macros for printing using RTE_LOG */
#define RTE_LOGTYPE_LSWITCH_CONFIG RTE_LOGTYPE_USER1
#define RTE_LOGTYPE_LSWITCH_DATA   RTE_LOGTYPE_USER1


/* State of virtio device. */
enum device_state {
	DEVICE_READY = 0,
	DEVICE_CLOSING = 1,
};


/* Device information */
struct dev_info {
	int virtual;    /* physical port (0) / virtual port (1) */
	int id;         /* portid / vid */
	volatile enum device_state state;

	TAILQ_ENTRY(dev_info) dev_entry;
} __rte_cache_aligned;
TAILQ_HEAD(dev_info_tailq, dev_info);

#endif
