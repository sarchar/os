#ifndef __EFIFB_H__
#define __EFIFB_H__

#include "common.h"

bool efifb_init(u32* framebuffer, u32 width, u32 height, u8 bits, u32 pitch);
void efifb_clear(color clear_color);
void efifb_putpixel(u32 x, u32 y, color c);

#endif
