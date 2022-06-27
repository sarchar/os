// A basic framebuffer device that makes use of the framebuffer provided by EFI
//
// Currently only compatible with a 32bpp framebuffer
//
#include "common.h"

#include "efifb.h"

struct efifb {
    u32* framebuffer;
    u32  width;
    u32  height;
    u8   bpp;
    u32  pitch;
};

static struct efifb global_efifb;

static bool efifb_iscompat(u32 width, u32 height, u8 bpp, u32 pitch)
{
    unused(height);

    if(width * (bpp >> 3) != pitch) return false;

    return true;
}

bool efifb_init(u32* framebuffer, u32 width, u32 height, u8 bpp, u32 pitch)
{
    global_efifb.framebuffer = framebuffer;
    global_efifb.width       = width;
    global_efifb.height      = height;
    global_efifb.bpp         = bpp;
    global_efifb.pitch       = pitch;
    return efifb_iscompat(width, height, bpp, pitch);
}

void efifb_putpixel(u32 x, u32 y, color c)
{
    if(x >= global_efifb.width || y >= global_efifb.height) return;
    global_efifb.framebuffer[y * global_efifb.width + x] = c;
}

void efifb_clear(color clear_color)
{
    for(u32 y = 0; y < global_efifb.height; y++) {
        for(u32 x = 0; x < global_efifb.width; x++) {
            global_efifb.framebuffer[y * global_efifb.width + x] = clear_color;
        }
    }
}

