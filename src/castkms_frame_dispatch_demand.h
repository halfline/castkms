/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_FRAME_DISPATCH_DEMAND_H_
#define _CASTKMS_FRAME_DISPATCH_DEMAND_H_

#include <linux/types.h>

/**
 * struct castkms_frame_dispatch_demand - Active frame consumers
 * @crc_enabled: Whether CRC capture requires frame dispatch
 * @writeback_count: Number of committed writeback jobs awaiting dispatch
 */
struct castkms_frame_dispatch_demand {
	bool crc_enabled;
	unsigned int writeback_count;
};

static inline bool castkms_frame_dispatch_demand_is_active(
	const struct castkms_frame_dispatch_demand *demand)
{
	return demand->crc_enabled || demand->writeback_count;
}

#endif /* _CASTKMS_FRAME_DISPATCH_DEMAND_H_ */
