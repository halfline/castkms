/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _CASTKMS_IOCTL_POLICY_H_
#define _CASTKMS_IOCTL_POLICY_H_

#include <linux/types.h>

bool castkms_ioctl_is_allowed_on_grant(unsigned int command);

#endif /* _CASTKMS_IOCTL_POLICY_H_ */
