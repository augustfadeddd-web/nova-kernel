#ifndef NOVA_LIBC_ERRNO_H
#define NOVA_LIBC_ERRNO_H

extern int errno;

#define EPERM 1
#define ENOENT 2
#define EINTR 4
#define EIO 5
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define EINVAL 22
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define ERANGE 34
#define ENOSYS 38
#define EDOM 33
#define EISDIR 21

#endif
