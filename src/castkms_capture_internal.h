/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_CAPTURE_INTERNAL_H_
#define _CASTKMS_CAPTURE_INTERNAL_H_

#include <linux/types.h>

#include "castkms_capture.h"

struct castkms_capture_stream {
	struct castkms_output *output;
	struct castkms_capture_authority *authority;
	u64 authority_generation;
	u64 mode_generation;
	bool active;
	bool attached;
};

#endif /* _CASTKMS_CAPTURE_INTERNAL_H_ */
