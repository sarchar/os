#include "common.h"

#include "cpu.h"
#include "interrupts.h"
#include "ps2keyboard.h"
#include "paging.h"
#include "palloc.h"
#include "stdio.h"
#include "stdlib.h"

#define KEYBOARD_DATA    0x60
#define KEYBOARD_STATUS  0x64
#define KEYBOARD_COMMAND 0x64

static struct {
    u8 *buffer;
    u8 head;
    u8 tail;
    u8 mod;
    ps2keyboard_cb* ascii_hook;
    void*           ascii_hook_userdata;
} kb_data = { 0, };

static void _kb_interrupt(struct interrupt_stack_registers* regs, intp pc, void* userdata)
{
    unused(regs);
    unused(pc);
    unused(userdata);

    // read the keyboard character
    kb_data.buffer[kb_data.tail] = __inb(KEYBOARD_DATA);
    kb_data.tail = (kb_data.tail + 1) % 4096;
}

void ps2keyboard_load()
{
    // set up the keyboard ring buffer
    // TODO free kb_data.buffer at some point?
    kb_data.buffer = (u8*)malloc(PAGE_SIZE); 

    // wait until there's no more data
    __inb(KEYBOARD_DATA);
    while(__inb(KEYBOARD_STATUS) & 0x02) __inb(KEYBOARD_DATA);

    interrupts_install_handler(33, _kb_interrupt, null);

    fprintf(stderr, "ps2keyboard: initialized\n");
}

struct keyboard_map_entry {
    u8 vk;
    u8 ascii;
};

static struct keyboard_map_entry keycode_map[128] = {
    [0x01] = { VK_ESCAPE     , ASCII_INVALID },
    [0x02] = { VK_1          , '1'           },
    [0x03] = { VK_2          , '2'           },
    [0x04] = { VK_3          , '3'           },
    [0x05] = { VK_4          , '4'           },
    [0x06] = { VK_5          , '5'           },
    [0x07] = { VK_6          , '6'           },
    [0x08] = { VK_7          , '7'           },
    [0x09] = { VK_8          , '8'           },
    [0x0A] = { VK_9          , '9'           },
    [0x0B] = { VK_0          , '0'           },
    [0x0C] = { VK_MINUS      , '-'           },
    [0x0D] = { VK_EQUALS     , '='           },
    [0x0E] = { VK_BACKSPACE  , ASCII_INVALID },
    [0x0F] = { VK_TAB        , '\t'          },
    [0x10] = { VK_Q          , 'q'           },
    [0x11] = { VK_W          , 'w'           },
    [0x12] = { VK_E          , 'e'           },
    [0x13] = { VK_R          , 'r'           },
    [0x14] = { VK_T          , 't'           },
    [0x15] = { VK_Y          , 'y'           },
    [0x16] = { VK_U          , 'u'           },
    [0x17] = { VK_I          , 'i'           },
    [0x18] = { VK_O          , 'o'           },
    [0x19] = { VK_P          , 'p'           },
    [0x1A] = { VK_LBRACKET   , '['           },
    [0x1B] = { VK_RBRACKET   , ']'           },
    [0x1C] = { VK_ENTER      , '\n'          },
    [0x1D] = { VK_LCONTROL   , ASCII_INVALID },
    [0x1E] = { VK_A          , 'a'           },
    [0x1F] = { VK_S          , 's'           },
    [0x20] = { VK_D          , 'd'           },
    [0x21] = { VK_F          , 'f'           },
    [0x22] = { VK_G          , 'g'           },
    [0x23] = { VK_H          , 'h'           },
    [0x24] = { VK_J          , 'j'           },
    [0x25] = { VK_K          , 'k'           },
    [0x26] = { VK_L          , 'l'           },
    [0x27] = { VK_SEMICOLON  , ';'           },
    [0x28] = { VK_SQUOTE     , '\''          },
    [0x29] = { VK_BACKTICK   , '`'           },
    [0x2A] = { VK_LSHIFT     , ASCII_INVALID },
    [0x2B] = { VK_LSLASH     , '\\'          },
    [0x2C] = { VK_Z          , 'z'           },
    [0x2D] = { VK_X          , 'x'           },
    [0x2E] = { VK_C          , 'c'           },
    [0x2F] = { VK_V          , 'v'           },
    [0x30] = { VK_B          , 'b'           },
    [0x31] = { VK_N          , 'n'           },
    [0x32] = { VK_M          , 'm'           },
    [0x33] = { VK_COMMA      , ','           },
    [0x34] = { VK_PERIOD     , '.'           },
    [0x35] = { VK_RSLASH     , '/'           },
    [0x36] = { VK_RSHIFT     , ASCII_INVALID },
    [0x37] = { VK_ASTERIX    , '*'           },
    [0x38] = { VK_LALT       , ASCII_INVALID },
    [0x39] = { VK_SPACE      , ' '           },
    [0x3A] = { VK_CAPSLOCK   , ASCII_INVALID },
    [0x3B] = { VK_F1         , ASCII_INVALID },
    [0x3C] = { VK_F2         , ASCII_INVALID },
    [0x3D] = { VK_F3         , ASCII_INVALID },
    [0x3E] = { VK_F4         , ASCII_INVALID },
    [0x3F] = { VK_F5         , ASCII_INVALID },
    [0x40] = { VK_F6         , ASCII_INVALID },
    [0x41] = { VK_F7         , ASCII_INVALID },
    [0x42] = { VK_F8         , ASCII_INVALID },
    [0x43] = { VK_F9         , ASCII_INVALID },
    [0x44] = { VK_F10        , ASCII_INVALID },
    [0x45] = { VK_NUM_LOCK   , ASCII_INVALID },
    [0x46] = { VK_SCROLL_LOCK, ASCII_INVALID },
    [0x47] = { VK_NP_7       , '7' },
    [0x48] = { VK_NP_8       , '8' },
    [0x49] = { VK_NP_9       , '9' },
    [0x4A] = { VK_NP_MINUS   , '-' },
    [0x4B] = { VK_NP_4       , '4' },
    [0x4C] = { VK_NP_5       , '5' },
    [0x4D] = { VK_NP_6       , '6' },
    [0x4E] = { VK_NP_PLUS    , '+' },
    [0x4F] = { VK_NP_1       , '1' },
    [0x50] = { VK_NP_2       , '2' },
    [0x51] = { VK_NP_3       , '3' },
    [0x52] = { VK_NP_0       , '0' },
    [0x53] = { VK_NP_PERIOD  , '.' },
    [0x57] = { VK_F11        , ASCII_INVALID },
    [0x58] = { VK_F12        , ASCII_INVALID },
};

static struct keyboard_map_entry keycode_map_shift[128] = {
    [0x01] = { VK_ESCAPE       , ASCII_INVALID },
    [0x02] = { VK_EXCLAMATION  , '!'           },
    [0x03] = { VK_AT           , '@'           },
    [0x04] = { VK_POUND        , '#'           },
    [0x05] = { VK_DOLLAR       , '$'           },
    [0x06] = { VK_PERCENT      , '%'           },
    [0x07] = { VK_CARET        , '^'           },
    [0x08] = { VK_AMPERSAND    , '&'           },
    [0x09] = { VK_ASTERIX      , '*'           },
    [0x0A] = { VK_LPAREN       , '('           },
    [0x0B] = { VK_RPAREN       , ')'           },
    [0x0C] = { VK_UNDERSCORE   , '_'           },
    [0x0D] = { VK_PLUS         , '+'           },
    [0x0E] = { VK_BACKSPACE    , ASCII_INVALID },
    [0x0F] = { VK_TAB          , '\t'          },
    [0x10] = { VK_Q            , 'Q'           },
    [0x11] = { VK_W            , 'W'           },
    [0x12] = { VK_E            , 'E'           },
    [0x13] = { VK_R            , 'R'           },
    [0x14] = { VK_T            , 'T'           },
    [0x15] = { VK_Y            , 'Y'           },
    [0x16] = { VK_U            , 'U'           },
    [0x17] = { VK_I            , 'I'           },
    [0x18] = { VK_O            , 'O'           },
    [0x19] = { VK_P            , 'P'           },
    [0x1A] = { VK_LBRACE       , '{'           },
    [0x1B] = { VK_RBRACE       , '}'           },
    [0x1C] = { VK_ENTER        , ASCII_INVALID },
    [0x1D] = { VK_LCONTROL     , ASCII_INVALID },
    [0x1E] = { VK_A            , 'A'           },
    [0x1F] = { VK_S            , 'S'           },
    [0x20] = { VK_D            , 'D'           },
    [0x21] = { VK_F            , 'F'           },
    [0x22] = { VK_G            , 'G'           },
    [0x23] = { VK_H            , 'H'           },
    [0x24] = { VK_J            , 'J'           },
    [0x25] = { VK_K            , 'K'           },
    [0x26] = { VK_L            , 'L'           },
    [0x27] = { VK_COLON        , ':'           },
    [0x28] = { VK_QUOTE        , '\"'          },
    [0x29] = { VK_TILDE        , '~'           },
    [0x2A] = { VK_LSHIFT       , ASCII_INVALID },
    [0x2B] = { VK_PIPE         , '|'           },
    [0x2C] = { VK_Z            , 'Z'           },
    [0x2D] = { VK_X            , 'X'           },
    [0x2E] = { VK_C            , 'C'           },
    [0x2F] = { VK_V            , 'V'           },
    [0x30] = { VK_B            , 'B'           },
    [0x31] = { VK_N            , 'N'           },
    [0x32] = { VK_M            , 'M'           },
    [0x33] = { VK_LANGLEBRACKET, '<'           },
    [0x34] = { VK_RANGLEBRACKET, '>'           },
    [0x35] = { VK_QUESTION_MARK, '?'           },
    [0x36] = { VK_RSHIFT       , ASCII_INVALID },
    [0x37] = { VK_NP_ASTERIX   , '*'           },
    [0x38] = { VK_LALT         , ASCII_INVALID },
    [0x39] = { VK_SPACE        , ' '           },
    [0x3A] = { VK_CAPSLOCK     , ASCII_INVALID },
    [0x3B] = { VK_F1           , ASCII_INVALID },
    [0x3C] = { VK_F2           , ASCII_INVALID },
    [0x3D] = { VK_F3           , ASCII_INVALID },
    [0x3E] = { VK_F4           , ASCII_INVALID },
    [0x3F] = { VK_F5           , ASCII_INVALID },
    [0x40] = { VK_F6           , ASCII_INVALID },
    [0x41] = { VK_F7           , ASCII_INVALID },
    [0x42] = { VK_F8           , ASCII_INVALID },
    [0x43] = { VK_F9           , ASCII_INVALID },
    [0x44] = { VK_F10          , ASCII_INVALID },
    [0x45] = { VK_NUM_LOCK     , ASCII_INVALID },
    [0x46] = { VK_SCROLL_LOCK  , ASCII_INVALID },
    [0x47] = { VK_NP_7         , '7' },
    [0x48] = { VK_NP_8         , '8' },
    [0x49] = { VK_NP_9         , '9' },
    [0x4A] = { VK_NP_MINUS     , '-' },
    [0x4B] = { VK_NP_4         , '4' },
    [0x4C] = { VK_NP_5         , '5' },
    [0x4D] = { VK_NP_6         , '6' },
    [0x4E] = { VK_NP_PLUS      , '+' },
    [0x4F] = { VK_NP_1         , '1' },
    [0x50] = { VK_NP_2         , '2' },
    [0x51] = { VK_NP_3         , '3' },
    [0x52] = { VK_NP_0         , '0' },
    [0x53] = { VK_NP_PERIOD    , '.' },
    [0x57] = { VK_F11          , ASCII_INVALID },
    [0x58] = { VK_F12          , ASCII_INVALID },
};

__always_inline static u8 _get_next_scancode() 
{
    u8 scancode = kb_data.buffer[kb_data.head];
    kb_data.head = (kb_data.head + 1) % 4096;
    return scancode;
}

void ps2keyboard_update()
{
    // nothing to do with no data
    if(kb_data.tail == kb_data.head) return;

    // process the data
    while(kb_data.tail != kb_data.head) {
        u8 scancode = _get_next_scancode();

        u8 up = (scancode & 0x80) != 0;
        scancode &= 0x7F;

        struct keyboard_map_entry* def = &keycode_map[scancode];
        if(def->vk == VK_LSHIFT || def->vk == VK_RSHIFT) {
            if(up) kb_data.mod &= ~0x01;
            else   kb_data.mod |= 0x01;
        }

        // map the shift keys
        if(kb_data.mod & 0x01) def = &keycode_map_shift[scancode];

        if(!up && def->ascii != ASCII_INVALID && kb_data.ascii_hook != null) {
            kb_data.ascii_hook(def->ascii, kb_data.ascii_hook_userdata);
        }
    }
}

void ps2keyboard_hook_ascii(ps2keyboard_cb* cb, void* userdata)
{
    kb_data.ascii_hook          = cb;
    kb_data.ascii_hook_userdata = userdata;
}
