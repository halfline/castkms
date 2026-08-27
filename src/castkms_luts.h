/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _CASTKMS_LUTS_H_
#define _CASTKMS_LUTS_H_

#include "castkms_frame.h"

#define LUT_SIZE 256

extern const struct castkms_color_lut castkms_linear_eotf;
extern const struct castkms_color_lut castkms_srgb_eotf;
extern const struct castkms_color_lut castkms_srgb_inv_eotf;

#endif /* _CASTKMS_LUTS_H_ */
