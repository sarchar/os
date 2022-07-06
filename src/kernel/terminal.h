#ifndef __TERMINAL_H__
#define __TERMINAL_H__

void terminal_init();
void terminal_setc(u16 c, u32 cx, u32 cy);
void terminal_putc(u16 c);
void terminal_step(u32 steps);
void terminal_scroll(u32 lines);
void terminal_redraw();

#endif
