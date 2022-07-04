// Display text to the framebuffer
//
#include "common.h"
#include "efifb.h"
#include "terminal.h"

// PSF font structure
// From https://wiki.osdev.org/PC_Screen_Font
// Better description of PSF1/2 formats: https://www.win.tue.nl/~aeb/linux/kbd/font-formats-1.html
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

#define TERMINAL_BACKLOG 2000
#define TERMINAL_VIRTUAL_Y(y) (((y) + current_terminal.window_y) % TERMINAL_BACKLOG)
#define TERMINAL_WIDTH   (1024/8)
#define TERMINAL_HEIGHT  (768/16)

struct terminal {
    u32 width;
    u32 height;
    u16 buffer[TERMINAL_WIDTH * TERMINAL_BACKLOG]; // 640/8 x 480/16 = 80x30 (TODO determine the size dynamically with framebuffer size)
    u32 cursor_x;
    u32 cursor_y;
    u32 window_y;
};

static struct terminal current_terminal;

static void _load_font()
{
//    u16 glyph = 0;

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

    current_terminal.width = TERMINAL_WIDTH; // TODO determine dynamically
    current_terminal.height = TERMINAL_HEIGHT;
    current_terminal.window_y = 0; // window starts at y=0
    current_terminal.cursor_x = 0;
    current_terminal.cursor_y = 0;
    // TODO memset
    for(u32 i = 0; i < countof(current_terminal.buffer); i++) current_terminal.buffer[i] = 0;
}

void terminal_setc(u16 c, u32 cx, u32 cy)
{
    current_terminal.buffer[TERMINAL_VIRTUAL_Y(cy) * current_terminal.width + cx] = c;

    if(c != 0) {
        _draw_char_to_framebuffer(c, cx * 8, cy * terminal_font->charsize, COLOR(255, 255, 255), COLOR(0, 0, 0));
    } else {
        for(u32 y = 0; y < terminal_font->charsize; y++) {
            for(u32 x = 0; x < 8; x++) {
                efifb_putpixel(cx * 8 + x, cy * terminal_font->charsize + y, COLOR(0, 0, 0));
            }
        }
    }

    //u32 xoffs = 0;
    //for(u32 i = 0; i < terminal_font->charsize; i++) {
    //    efifb_putpixel(xoffs + 10, 160, COLOR(255, 255, 255));
    //    efifb_putpixel(xoffs + 10, 161, COLOR(255, 255, 255));
    //    efifb_putpixel(xoffs + 11, 160, COLOR(255, 255, 255));
    //    efifb_putpixel(xoffs + 11, 161, COLOR(255, 255, 255));
    //    xoffs += 8;
    //}
}

void terminal_putc(u16 c)
{
    if(c == '\n') {
        current_terminal.cursor_x = 0;
        current_terminal.cursor_y += 1;
        if(current_terminal.cursor_y >= current_terminal.height) {
            current_terminal.cursor_y = current_terminal.height - 1;
            terminal_scroll(1);
        }
    } else {
        terminal_setc(c, current_terminal.cursor_x, current_terminal.cursor_y);
        terminal_step(1);
    }
}

// move the cursor forward n steps without changing the terminal buffer
// usually only stepped by 1 after printing a character
void terminal_step(u32 steps)
{
    while(steps-- != 0) {
        current_terminal.cursor_x += 1;
        if(current_terminal.cursor_x >= current_terminal.width) {
            current_terminal.cursor_x = 0;
            current_terminal.cursor_y += 1;
            if(current_terminal.cursor_y >= current_terminal.height) {
                current_terminal.cursor_y = current_terminal.height - 1;
                terminal_scroll(1);
            }
        }
    }
}

void terminal_scroll(u32 lines)
{
    current_terminal.window_y += lines;
    terminal_redraw();
}

void terminal_redraw()
{
    for(u32 y = 0; y < current_terminal.height; y++) {
        for(u32 x = 0; x < current_terminal.width; x++) {
            u8 c = current_terminal.buffer[TERMINAL_VIRTUAL_Y(y) * current_terminal.width + x];
            terminal_setc(c, x, y);
        }
    }
}

// functions below this line are utility print functions that will ideally be replaced with a printf function
//
static char const HEX_LETTERS[] = "0123456789ABCDEF";

void terminal_print_string(char* s)
{
    while(*s != '\0') {
        // TODO decode utf8??
        terminal_putc((u16)*s++);
    }
}

void terminal_print_stringnl(char* s)
{
    terminal_print_string(s);
    terminal_putc(L'\n');
}


void terminal_print_pointer(void* a)
{
    // don't display the upper long if it's all zeros
    u32 loop_size = (((u64)a & 0xFFFFFFFF00000000LL) == 0) ? 8 : 16;

    for(u32 i = 0; i < loop_size; i++) {
        u8 v = (((long long)a) >> (((loop_size - 1) - i) * 4)) & 0x0F;
        terminal_putc((u16)HEX_LETTERS[v]);
    }
}

void terminal_print_u64(u64 v)
{
    for(u32 i = 0; i < 16; i++) {
        u8 t = (v >> ((15 - i) * 4)) & 0x0F;
        terminal_putc((u16)HEX_LETTERS[t]);
    }
}

void terminal_print_u32(u32 v)
{
    for(u32 i = 0; i < 8; i++) {
        u8 t = (v >> ((7 - i) * 4)) & 0x0F;
        terminal_putc((u16)HEX_LETTERS[t]);
    }
}

void terminal_print_u16(u16 v)
{
    for(u32 i = 0; i < 4; i++) {
        u8 t = (v >> ((3 - i) * 4)) & 0x0F;
        terminal_putc((u16)HEX_LETTERS[t]);
    }
}

void terminal_print_u8(u8 v)
{
    for(u32 i = 0; i < 2; i++) {
        u8 t = (v >> ((1 - i) * 4)) & 0x0F;
        terminal_putc((u16)HEX_LETTERS[t]);
    }
}

