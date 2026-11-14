#ifndef NOVA_LIBC_SYS_TYPES_H
#define NOVA_LIBC_SYS_TYPES_H

#include <stddef.h>

typedef long ssize_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned int mode_t;
typedef unsigned int useconds_t;
typedef long time_t;
typedef long clock_t;
typedef unsigned long ino_t;
typedef unsigned long dev_t;
typedef unsigned long nlink_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned long st_dev_t;

#define CLOCK_TICKS_PER_SECOND 100

#endif
