/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_WRITEBACK_H_
#define _CASTKMS_WRITEBACK_H_

struct castkms_device;
struct castkms_output;

int castkms_enable_writeback_connector(struct castkms_device *castkmsdev,
				       struct castkms_output *output);

#endif /* _CASTKMS_WRITEBACK_H_ */
