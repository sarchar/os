#ifndef __BUFFER_H__
#define __BUFFER_H__

// Not terribly happy with placing this buffer-thing here, as I'm not sure what to call it and what it might be
// actually useful for in the future.  That being said,
//
// These buffers are generally for I/O receive and transmit queues like network sockets and disk stores, but 
// I imagine they may be useful for many things going forward.  They're very basic right now, but get the 
// job done.
//
// The implementation of the read/write system a single buffer object is a ring buffer, and data can
// be continually read and written to them.  At the same time, you can form a linked list of them if 
// they're only used as unidirection buffers.
//
// Currently, these buffers are NOT thread-safe
#include "deque.h"

struct buffer {
    MAKE_DEQUE(struct buffer);    // 16 bytes

    u8* buf;       // vmem pointer to actual data storage
    u32 read_pos;  // current read position into buf
    u32 write_pos; // current write position into buf

    u32 size;      // allocated size for buf
    u32 usage;     // current amount of data in the buffer

    u32 buf_flags; // flags used by buffer_ module
    u32 flags;     // generic flags to be used with whatever module needs it

    u64 unused0;   // to align to 64 bytes
    u64 unused1;
};

struct buffer* buffer_create(u32 size);
struct buffer* buffer_create_with(u8*, u32 size, u32 write_pos);     // provide your own storage
void           buffer_destroy(struct buffer*);
u32            buffer_peek(struct buffer*, u8*, u32); // read without increasing read_pos
u32            buffer_read(struct buffer*, u8*, u32); // dest can be null, which just increases read_pos without copying data
u32            buffer_read_into(struct buffer*, struct buffer*, u32);
u32            buffer_write(struct buffer*, u8*, u32);
u32            buffer_write_from(struct buffer*, struct buffer*, u32);
u32            buffer_puts(struct buffer*, char*);

__always_inline u32 buffer_remaining_read(struct buffer* buf) { return buf->usage; }
__always_inline u32 buffer_remaining_write(struct buffer* buf) { return buf->size - buf->usage; } 

#endif
