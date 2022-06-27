// Display text to the framebuffer
//
#include "common.h"
#include "efifb.h"

// PSF font structure
// From https://wiki.osdev.org/PC_Screen_Font
#define PSF_FONT_MAGIC 0x864AB572
 
struct psf1_font {
    u8 magic[2];       // Magic number
    u8 mode;           // PSF font mode
    u8 charsize;       // Character size
};

// reference our psf font embedded in the kernel
extern u8 _binary_font_psf_start;
extern u8 _binary_font_psf_end;

// cast the address to PSF header struct
struct psf1_font *terminal_font;

static u16 unicode_map[1 << 16]; // 16-bit mapping for unicode TODO use calloc?
static bool font_has_unicode;

static void _load_font()
{
    u16 glyph = 0;

    // cast the address to PSF header struct
    terminal_font = (struct psf1_font*)&_binary_font_psf_start;
    font_has_unicode = false;
return;
#if 0
    font_has_unicode = (terminal_font->flags == 0);

    // is there a unicode table?
    if (!font_has_unicode) return; 

    // get the offset of the table
    u8 *s = &_binary_font_psf_start + terminal_font->headersize + terminal_font->numglyph * terminal_font->bytesperglyph;

    // allocate memory for translation table
    //TODO unicode = calloc(USHRT_MAX, 2);

    while(s > (u8*)&_binary_font_psf_end) {
        u16 uc = (u16)s[0];
        if(uc == 0xFF) {
            glyph++;
            s++;
            continue;
        } else if (uc & 0x80) {
            // UTF-8 to unicode 
            if ((uc & 0x20) == 0) {
                uc = ((s[0] & 0x1F) << 6) + (s[1] & 0x3F);
                s++;
            } else {
                if ((uc & 0x10) == 0) {
                    uc = ((((s[0] & 0xF) << 6) + (s[1] & 0x3F)) << 6) + (s[2] & 0x3F);
                    s += 2;
                } else {
                    if ((uc & 0x08) == 0) {
                        uc = ((((((s[0] & 0x7) << 6) + (s[1] & 0x3F)) << 6) + (s[2] & 0x3F)) << 6) + (s[3] & 0x3F);
                        s += 3;
                    } else {
                        uc = 0;
                    }
                }
            }
        }

        // save translation 
        unicode_map[uc] = glyph;
        s++;
    }
#endif
}

static void _draw_char_to_framebuffer(u16 c, u32 x, u32 y, color text_color, color bg_color)
{
    if(c >= 256) return;

    // we need to know how many bytes encode one row
    //u8 bytesperline = (terminal_font->width + 7) / 8;
    u8 bytesperline = 1;

    // unicode translation 
    if(font_has_unicode) c = unicode_map[c];

    // get the glyph for the character. If there's no glyph for a given character, we'll display the first glyph.
    //u32 *glyph = (u32*)((u8*)&_binary_font_psf_start + terminal_font->headersize + ((c >= 0 && c < terminal_font->numglyph) ? c : 0) * terminal_font->bytesperglyph);
    u8 *glyph = (u8*)terminal_font + sizeof(struct psf1_font) + c * terminal_font->charsize * bytesperline;

    // finally display pixels according to the bitmap
    for(u32 cy = 0; cy < terminal_font->charsize; cy++) {
        u32 mask = 1 << 7;

        // display a row
        for(u32 cx = 0; cx < 8; cx++) {
            efifb_putpixel(x + cx, y + cy, ((*glyph & mask) != 0) ? text_color : bg_color);
            mask >>= 1;
        }

        // adjust to the next line
        glyph += bytesperline;
    }
}

void terminal_init()
{
    _load_font();
}

void terminal_setc(u16 c, u32 cx, u32 cy)
{
    _draw_char_to_framebuffer(c, cx * 8, cy * terminal_font->charsize, COLOR(255, 255, 255), COLOR(0, 0, 0));

    //u32 xoffs = 0;
    //for(u32 i = 0; i < terminal_font->charsize; i++) {
    //    efifb_putpixel(xoffs + 10, 160, COLOR(255, 255, 255));
    //    efifb_putpixel(xoffs + 10, 161, COLOR(255, 255, 255));
    //    efifb_putpixel(xoffs + 11, 160, COLOR(255, 255, 255));
    //    efifb_putpixel(xoffs + 11, 161, COLOR(255, 255, 255));
    //    xoffs += 8;
    //}
}

