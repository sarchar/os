#ifndef __EFIFB_H__
#define __EFIFB_H__

#include "common.h"

void efifb_init();
void efifb_clear(color clear_color);
void efifb_putpixel(u32 x, u32 y, color c);

#endif
