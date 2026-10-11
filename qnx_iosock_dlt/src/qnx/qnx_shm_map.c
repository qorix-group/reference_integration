/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/
/*
 * Copyright (c) 2026 Qorix
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Maps the shared-memory ring. No #include of system headers on purpose, see qnx_shm_map.h: the
 * libc functions are declared here by hand with the QNX 8.0 prototypes and constants
 * (usr/include/fcntl.h, sys/mman.h, unistd.h, sys/stat.h). Both supported targets (x86_64,
 * aarch64) are LP64, so off_t and mode_t are 64 and 32 bit wide.
 */

#include "qnx_shm_map.h"

/* QNX values, they differ from the FreeBSD ones used by the kernel headers. */
#define QNX_O_RDWR 0x0002
#define QNX_O_CREAT 0x0100 /* 000400 octal */
#define QNX_O_EXCL 0x0400  /* 002000 octal */
#define QNX_PROT_READ 0x00000100
#define QNX_PROT_WRITE 0x00000200
#define QNX_MAP_SHARED 0x00000001
#define QNX_MAP_FAILED ((void *)-1)
#define QNX_EIO 5
#define QNX_MODE_RW_ALL 0666U

extern int shm_open(const char *name, int oflag, unsigned int mode);
extern int shm_unlink(const char *name);
extern int ftruncate(int fd, long length);
extern int fchmod(int fd, unsigned int mode);
extern void *mmap(void *addr, dlt_size length, int prot, int flags, int fd, long offset);
extern int munmap(void *addr, dlt_size length);
extern int close(int fd);
extern int *__get_errno_ptr(void);

int
dlt_shm_map_create(const char *name, dlt_size size, void **base_out)
{
	void *mem;
	int fd;
	int error;

	if ((name == 0) || (size == 0) || (base_out == 0)) {
		return (22); /* EINVAL */
	}

	(void)shm_unlink(name); /* a stale object from an earlier run is replaced */
	fd = shm_open(name, QNX_O_RDWR | QNX_O_CREAT | QNX_O_EXCL, QNX_MODE_RW_ALL);
	if (fd < 0) {
		error = *__get_errno_ptr();
		return ((error != 0) ? error : QNX_EIO);
	}
	(void)fchmod(fd, QNX_MODE_RW_ALL); /* shm_open applies the umask */

	if (ftruncate(fd, (long)size) != 0) {
		error = *__get_errno_ptr();
		(void)close(fd);
		(void)shm_unlink(name);
		return ((error != 0) ? error : QNX_EIO);
	}

	mem = mmap(0, size, QNX_PROT_READ | QNX_PROT_WRITE, QNX_MAP_SHARED, fd, 0);
	if (mem == QNX_MAP_FAILED) {
		error = *__get_errno_ptr();
		(void)close(fd);
		(void)shm_unlink(name);
		return ((error != 0) ? error : QNX_EIO);
	}

	(void)close(fd); /* the mapping keeps the object alive */
	*base_out = mem;
	return (0);
}
