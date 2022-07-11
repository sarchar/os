// A basic framebuffer device that makes use of the framebuffer provided by EFI
//
// Currently only compatible with a 32bpp framebuffer
//
#include "common.h"

#include "efifb.h"
#include "kernel.h"
#include "multiboot2.h"
#include "terminal.h"

struct efifb {
    u32* framebuffer;
    u32  width;
    u32  height;
    u8   bpp;
    u32  pitch;
};

static struct efifb global_efifb = { NULL, 0, 0, 0, 0 };

static bool efifb_iscompat(u32 width, u32 height, u8 bpp, u32 pitch)
{
    unused(height);

    if(width * (bpp >> 3) != pitch) return false;

    return true;
}

void efifb_init()
{
    multiboot2_framebuffer_get(&global_efifb.framebuffer,
            &global_efifb.width,
            &global_efifb.height,
            &global_efifb.bpp,
            &global_efifb.pitch);

    // halt if framebuffer isn't valid
    if(!efifb_iscompat(global_efifb.width, global_efifb.height, global_efifb.bpp, global_efifb.pitch)) PANIC(COLOR(0,0,0));

    // set screen blue to show that we have initialized the frame buffer
    efifb_clear(COLOR(0, 0, 255));
    
    // wait for some arbitrary amount of time so that the blue screen is visible
    // sleep() isn't available right now
    for(int i = 0; i < 3000000; i++) asm volatile("pause");
    
    // and clear it to black before proceeding
    efifb_clear(COLOR(0, 0, 0));
    
    // now that there's a working framebuffer, refresh the terminal to it
    terminal_redraw();
    
    // draw an 8x8 square in the corner to indicate that we made it this far
    for(u32 y = 0; y < 8; y++) {
        for(u32 x = 0; x < 8; x++) {
            efifb_putpixel(x + global_efifb.width - 16,
                           y + global_efifb.height - 16,
                           COLOR(0, 255, 0));
        }
    }
}

void efifb_putpixel(u32 x, u32 y, color c)
{
    if(x >= global_efifb.width || y >= global_efifb.height) return;
    if(global_efifb.framebuffer != NULL) {
        global_efifb.framebuffer[y * global_efifb.width + x] = c;
    }
}

void efifb_clear(color clear_color)
{
    for(u32 y = 0; y < global_efifb.height; y++) {
        for(u32 x = 0; x < global_efifb.width; x++) {
            global_efifb.framebuffer[y * global_efifb.width + x] = clear_color;
        }
    }
}

