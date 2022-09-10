// A basic framebuffer device that makes use of the framebuffer provided by EFI
//
// Currently only compatible with a 32bpp framebuffer
//
#include "common.h"

#include "efifb.h"
#include "kernel.h"
#include "multiboot2.h"
#include "paging.h"
#include "terminal.h"
#include "string.h"

struct efifb {
    u32* framebuffer;
    u32  width;
    u32  height;
    u8   bpp;
    u32  pitch;
    u8   type;
    u8   disabled;
} __packed;

static struct efifb global_efifb = { 0, };

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
            &global_efifb.pitch,
            &global_efifb.type);

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
    terminal_redraw(0);
    
    // draw an 8x8 square in the corner to indicate that we made it this far
    for(u32 y = 0; y < 8; y++) {
        for(u32 x = 0; x < 8; x++) {
            efifb_putpixel(x + global_efifb.width - 16,
                           y + global_efifb.height - 16,
                           COLOR(0, 255, 0));
        }
    }
}

void efifb_disable()
{
    global_efifb.disabled = true;
}

// called after kernel paging takes over so that we know to remap the efi framebuffer
// identity map the frame buffer. it should be in lowmem
void efifb_map()
{
    global_efifb.disabled = true;
    intp region_start = (intp)global_efifb.framebuffer;
    u64 region_size = (u64)__alignup((intp)(global_efifb.pitch * global_efifb.height), 4096);
    paging_identity_map_region(PAGING_KERNEL, region_start, region_size, MAP_PAGE_FLAG_WRITABLE | MAP_PAGE_FLAG_DISABLE_CACHE);
    global_efifb.disabled = false;
}

void efifb_putpixel(u32 x, u32 y, color c)
{
    if(global_efifb.disabled) return;
    if(x >= global_efifb.width || y >= global_efifb.height) return;
    if(global_efifb.framebuffer != NULL) {
        if(global_efifb.type == 1) {
            u8* p = &((u8*)global_efifb.framebuffer)[y * global_efifb.pitch + 3*x];
            p[0] = c & 0xFF;
            p[1] = (c >> 8) & 0xFF;
            p[2] = (c >> 16) & 0xFF;
        } else {
            global_efifb.framebuffer[y * global_efifb.width + x] = c;
        }
    }
}

void efifb_clear(color clear_color)
{
    if(global_efifb.type) {
        u8* fb = (u8*)global_efifb.framebuffer;
        for(u32 i = 0; i < (global_efifb.height * global_efifb.width); i++) {
            *fb++ = clear_color & 0xFF;
            *fb++ = (clear_color >> 8) & 0xFF;
            *fb++ = (clear_color >> 16) & 0xFF;
        }
    } else {
        u32* fb = global_efifb.framebuffer;
        for(u32 i = 0; i < (global_efifb.height * global_efifb.width); i++) {
            *fb++ = clear_color;
        }
    }
}

// scroll the entire screen up by y pixels
void efifb_scroll(u32 y) 
{
    if(global_efifb.disabled) return;
    u8* fb = (u8*)global_efifb.framebuffer;
    memcpy(fb, &fb[y * global_efifb.pitch], (global_efifb.height - y) * global_efifb.pitch);
}
