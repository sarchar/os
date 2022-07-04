#ifndef __TERMINAL_H__
#define __TERMINAL_H__

void terminal_init();
void terminal_setc(u16 c, u32 cx, u32 cy);
void terminal_putc(u16 c);
void terminal_step(u32 steps);
void terminal_scroll(u32 lines);
void terminal_redraw();

// print functions
void terminal_print_string(char* s);
void terminal_print_stringnl(char* s);
void terminal_print_pointer(void* a);
void terminal_print_u64(u64 v);
void terminal_print_u32(u32 v);
void terminal_print_u16(u16 v);
void terminal_print_u8(u8 v);

#endif
