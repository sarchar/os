#include "common.h"

#include "buffer.h"
#include "kalloc.h"
#include "kernel.h"
#include "stdlib.h"
#include "string.h"

struct buffer* buffer_create(u32 size)
{
    struct buffer* buf = (struct buffer*)kalloc(sizeof(struct buffer));
    zero(buf);
    buf->buf = (u8*)malloc(size);
    buf->size = size;
    return buf;
}

void buffer_destroy(struct buffer* buf)
{
    free(buf->buf);
    kfree(buf, sizeof(struct buffer));
}

u32 buffer_read(struct buffer* buf, u8* dest, u32 max_read_size)
{
    u32 total_read_size = 0;

    if(buf->usage > 0 && buf->read_pos >= buf->write_pos) {
        u32 read_size = min(buf->size - buf->read_pos, max_read_size);
        memcpy(dest, &buf->buf[buf->read_pos], read_size);
        dest += read_size;
        max_read_size -= read_size;
        total_read_size += read_size;
        buf->read_pos = (buf->read_pos + read_size) % buf->size;
        buf->usage -= read_size;
    }

    if(buf->read_pos < buf->write_pos) { // implies buf->usage > 0
        u32 read_size = min(buf->write_pos - buf->read_pos, max_read_size);
        memcpy(dest, &buf->buf[buf->read_pos], read_size);
        total_read_size += read_size;
        buf->read_pos += read_size;
        buf->usage -= read_size;
    }

    return total_read_size;
}

u32 buffer_read_into(struct buffer* dest, struct buffer* src, u32 count)
{
    // might have to split the write into two, to wrap around the ring buffer
    u32 remaining_read = buffer_remaining_read(src);
    count = min(remaining_read, count); // truncate to incoming buffer length

    u32 first_read = min(count, dest->size - dest->write_pos); // first write to the end of the buffer
    u32 v = buffer_write(dest, &src->buf[src->read_pos], first_read);
    assert(v == first_read, "read should have been complete");
    src->read_pos = (src->read_pos + v) % src->size;
    src->usage -= v;

    u32 second_read = count - first_read; // remaining data comes from the beginning of the ring buffer
    v = buffer_write(dest, &src->buf[src->read_pos], second_read);
    assert(v == second_read, "read should have been complete");
    src->read_pos = (src->read_pos + v) % src->size;
    src->usage -= v;

    return first_read + second_read;
}

u32 buffer_write(struct buffer* buf, u8* src, u32 count)
{
    u32 total_write_size = 0;

    if(buf->usage < buf->size && buf->write_pos >= buf->read_pos) {
        u32 write_size = min(buf->size - buf->write_pos, count);
        memcpy(&buf->buf[buf->write_pos], src, write_size);
        src += write_size;
        count -= write_size;
        total_write_size += write_size;
        buf->write_pos = (buf->write_pos + write_size) % buf->size;
        buf->usage += write_size;
    }

    if(buf->write_pos < buf->read_pos) { // implies buf->usage < buf->size
        u32 write_size = min(buf->read_pos - buf->write_pos, count);
        memcpy(&buf->buf[buf->write_pos], src, write_size);
        total_write_size += write_size;
        buf->write_pos += write_size;
        buf->usage += write_size;
    }

    return total_write_size;
}

u32 buffer_write_from(struct buffer* dest, struct buffer* src, u32 count)
{
    // use buffer_read()
    assert(false, "TODO write me");
    return 0;
}

u32 buffer_puts(struct buffer* buf, char* s)
{
    u32 ret = 0;
    while(buf->usage < buf->size && *s != 0) {
        buf->buf[buf->write_pos] = *s++;
        buf->write_pos = (buf->write_pos + 1) % buf->size;
        buf->usage++;
        ret++;
    }
    return ret;
}
