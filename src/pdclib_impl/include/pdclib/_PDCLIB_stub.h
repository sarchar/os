#ifndef _PDCLIB_STUB_H
#define _PDCLIB_STUB_H

#ifdef LACKS_SYS_TYPES_H
typedef _PDCLIB_size_t size_t;
#endif

#define O_RDWR 0
#define O_RDONLY 0
#define O_CREAT 0
#define O_EXCL 0
#define S_IRUSR 0
#define S_IWUSR 0
#define PROT_READ 0
#define PROT_WRITE 0
#define MAP_PRIVATE 0

extern int errno;
#define ENOMEM -1
#define EINVAL -1
#define ETIMEDOUT -1
#define EBUSY -1
#define EINTR -1

#endif

