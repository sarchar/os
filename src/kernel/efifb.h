#ifndef __EFIFB_H__
#define __EFIFB_H__

#include "common.h"

typedef u32 color;

#define COLOR(r,g,b) (color)(0x00000000 | ((r) << 16) | ((g) << 8) | (b))

bool efifb_init(u32* framebuffer, u32 width, u32 height, u8 bits, u32 pitch);
void efifb_clear(color clear_color);
void efifb_putpixel(u32 x, u32 y, color c);

#endif
