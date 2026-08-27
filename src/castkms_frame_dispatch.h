/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_FRAME_DISPATCH_H_
#define _CASTKMS_FRAME_DISPATCH_H_

#include <linux/kconfig.h>
#include <linux/types.h>

#include "castkms_frame_dispatch_demand.h"

struct castkms_output;
struct work_struct;

enum castkms_frame_dispatch_client {
	CASTKMS_FRAME_DISPATCH_CLIENT_CRC,
	CASTKMS_FRAME_DISPATCH_CLIENT_WRITEBACK,
};

void castkms_frame_dispatch_worker(struct work_struct *work);
int castkms_frame_dispatch_get(struct castkms_output *output,
			       enum castkms_frame_dispatch_client client);
void castkms_frame_dispatch_put(struct castkms_output *output,
				enum castkms_frame_dispatch_client client);

#if IS_ENABLED(CONFIG_KUNIT)
int castkms_frame_dispatch_demand_get(
	struct castkms_frame_dispatch_demand *demand,
	enum castkms_frame_dispatch_client client,
	int vblank_ret, bool *keep_vblank);
bool castkms_frame_dispatch_demand_put(
	struct castkms_frame_dispatch_demand *demand,
	enum castkms_frame_dispatch_client client);
#endif

#endif /* _CASTKMS_FRAME_DISPATCH_H_ */
