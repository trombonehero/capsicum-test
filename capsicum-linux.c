/*-
 * Copyright (c) 2013 FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Pawel Jakub Dawidek under sponsorship from
 * the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef __linux__
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "capsicum-linux.h"

/************************************************************
 * Capsicum System Calls.
 ************************************************************/
int cap_enter() {
  int rc = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
  if (rc < 0) return rc;
  return prctl(PR_SET_SECCOMP, SECCOMP_MODE_LSM);
}

int cap_getmode(unsigned int *mode) {
  int rc = prctl(PR_GET_SECCOMP);
  if (rc < 0) return rc;
  *mode = (rc == SECCOMP_MODE_LSM);
  return 0;
}

static inline unsigned int
right_to_index(uint64_t right)
{
	static const int bit2idx[] = {
		-1, 0, 1, -1, 2, -1, -1, -1, 3, -1, -1, -1, -1, -1, -1, -1,
		4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
	};
	int idx;

	idx = CAPIDXBIT(right);
	assert(idx == 1 || idx == 2 || idx == 4 || idx == 8 || idx == 16);

	idx = bit2idx[idx];
	assert(idx >= 0 && idx <= 4);

	return ((unsigned int)idx);
}

static inline bool has_right(const cap_rights_t *rights, __u64 right)
{
	int idx = right_to_index(right);
	return (rights->cr_rights[idx] & right) == right;
}

static void print_rights_all(FILE *f,
                             const cap_rights_t *rights,
                             unsigned int fcntls,
                             int nioctls,
                             const unsigned int *ioctls) {
  int ii;
  fprintf(f, "%016llx %016llx fcntls=%08x |%d|",
          rights->cr_rights[0], rights->cr_rights[1], fcntls, nioctls);
  if (ioctls) {
    for (ii = 0; ii < nioctls; ii++) {
      fprintf(f, " %08x", ioctls[ii]);
    }
  }
  fprintf(f, "\n");
}

static void cap_rights_regularize(const cap_rights_t * rights,
                                  unsigned int *fcntls,
                                  int *nioctls,
                                  unsigned int **ioctls) {
  if (!has_right(rights, CAP_FCNTL)) {
    *fcntls = 0x00;
  }
  if (!has_right(rights, CAP_IOCTL)) {
    *nioctls = 0;
    free(*ioctls);
    *ioctls = NULL;
  }
}

/* Caller owns (*ioctls) on return */
static int cap_rights_get_all(int fd,
                              cap_rights_t *rights,
                              unsigned int *fcntls,
                              int *nioctls,
                              unsigned int **ioctls) {
  int rc;
  syscall(__NR_cap_rights_get, fd, rights, fcntls, nioctls, NULL);
  if (ioctls) {
    if (*nioctls > 0) {
      *ioctls = malloc(*nioctls * sizeof(unsigned int));
      if (*ioctls == NULL) {
        errno = ENOMEM;
        return -1;
      }
      syscall(__NR_cap_rights_get, fd, NULL, NULL, nioctls, *ioctls);
    } else {
      *ioctls = NULL;
    }
  }
  return 0;
}

int cap_rights_limit(int fd, const cap_rights_t *rights) {
  cap_rights_t primary;
  unsigned int fcntls;
  int nioctls;
  unsigned int *ioctls = NULL;
  int rc;
  rc = cap_rights_get_all(fd, &primary, &fcntls, &nioctls, &ioctls);
  if (rc) {
    return rc;
  }
  cap_rights_regularize(rights, &fcntls, &nioctls, &ioctls);
  rc = syscall(__NR_cap_rights_limit, fd, rights, fcntls, nioctls, ioctls);
  if (ioctls) {
    free(ioctls);
  }
  return rc;
}

int cap_rights_get(int fd, cap_rights_t *rights) {
  return syscall(__NR_cap_rights_get, fd, rights, NULL, NULL, NULL);
}

int cap_fcntls_limit(int fd, cap_fcntl_t fcntls) {
  cap_rights_t primary;
  int nioctls;
  unsigned int prev_fcntls;
  unsigned int *ioctls = NULL;
  int rc;
  rc = cap_rights_get_all(fd, &primary, &prev_fcntls, &nioctls, &ioctls);
  if (rc) {
    return rc;
  }
  rc = syscall(__NR_cap_rights_limit, fd, &primary, fcntls, nioctls, ioctls);
  if (ioctls) {
    free(ioctls);
  }
  return rc;
}

int cap_fcntls_get(int fd, cap_fcntl_t *fcntlsp) {
  return syscall(__NR_cap_rights_get, fd, NULL, fcntlsp, NULL, NULL);
}

int cap_ioctls_limit(int fd, const unsigned int *cmds, size_t ncmds) {
  cap_rights_t primary;
  unsigned int fcntls;
  int prev_nioctls;
  int rc;
  rc = cap_rights_get_all(fd, &primary, &fcntls, &prev_nioctls, NULL);
  if (rc) {
    return rc;
  }
  rc = syscall(__NR_cap_rights_limit, fd, &primary, fcntls, ncmds, cmds);
  return rc;
}

ssize_t cap_ioctls_get(int fd, unsigned int *cmds, size_t maxcmds) {
  int n = maxcmds;
  int rc = syscall(__NR_cap_rights_get, fd, NULL, NULL, &n, cmds);
  if (rc >= 0) {
    return (n == -1) ? CAP_IOCTLS_ALL : n;
  } else {
    return rc;
  }
}

/* Linux glibc includes an fexecve() function, implemented via the /proc
 * filesystem.  Bypass this and go directly to the fexecve syscall.
 */
int fexecve_(int fd, char **argv, char **envp)
{
  return syscall(__NR_fexecve, fd, argv, envp);
}

int pdfork(int *fd, int flags) {
  return syscall(__NR_pdfork, fd, flags);
}

int pdgetpid(int fd, pid_t *pid) {
  return syscall(__NR_pdgetpid, fd, pid);
}

int pdkill(int fd, int signum) {
  return syscall(__NR_pdkill, fd, signum);
}

int pdwait4(int fd, int *status, int options, struct rusage *rusage) {
  return syscall(__NR_pdwait4, fd, status, options, rusage);
}

static void
cap_rights_vset(cap_rights_t *rights, va_list ap)
{
	unsigned int i, n;
	uint64_t right;

	assert(CAPVER(rights) == CAP_RIGHTS_VERSION_00);

	n = CAPARSIZE(rights);

	for (;;) {
		right = (uint64_t)va_arg(ap, unsigned long long);
		if (right == 0)
			break;
		assert(CAPRVER(right) == 0);
		i = right_to_index(right);
		assert(i < n);
		assert(CAPIDXBIT(rights->cr_rights[i]) == CAPIDXBIT(right));
		rights->cr_rights[i] |= right;
		assert(CAPIDXBIT(rights->cr_rights[i]) == CAPIDXBIT(right));
	}
}

static void
cap_rights_vclear(cap_rights_t *rights, va_list ap)
{
	unsigned int i, n;
	uint64_t right;

	assert(CAPVER(rights) == CAP_RIGHTS_VERSION_00);

	n = CAPARSIZE(rights);

	for (;;) {
		right = (uint64_t)va_arg(ap, unsigned long long);
		if (right == 0)
			break;
		assert(CAPRVER(right) == 0);
		i = right_to_index(right);
		assert(i < n);
		assert(CAPIDXBIT(rights->cr_rights[i]) == CAPIDXBIT(right));
		rights->cr_rights[i] &= ~(right & 0x01FFFFFFFFFFFFFFULL);
		assert(CAPIDXBIT(rights->cr_rights[i]) == CAPIDXBIT(right));
	}
}

static bool
cap_rights_is_vset(const cap_rights_t *rights, va_list ap)
{
	unsigned int i, n;
	uint64_t right;

	assert(CAPVER(rights) == CAP_RIGHTS_VERSION_00);

	n = CAPARSIZE(rights);

	for (;;) {
		right = (uint64_t)va_arg(ap, unsigned long long);
		if (right == 0)
			break;
		assert(CAPRVER(right) == 0);
		i = right_to_index(right);
		assert(i < n);
		assert(CAPIDXBIT(rights->cr_rights[i]) == CAPIDXBIT(right));
		if ((rights->cr_rights[i] & right) != right)
			return (false);
	}

	return (true);
}

cap_rights_t *
_cap_rights_init(int version, cap_rights_t *rights, ...)
{
	unsigned int n;
	va_list ap;

	assert(version == CAP_RIGHTS_VERSION_00);

	n = version + 2;
	memset(rights->cr_rights, 0, sizeof(rights->cr_rights[0]) * n);
	CAP_SET_NONE(rights);
	va_start(ap, rights);
	cap_rights_vset(rights, ap);
	va_end(ap);

	return (rights);
}

void
_cap_rights_set(cap_rights_t *rights, ...)
{
	va_list ap;

	assert(CAPVER(rights) == CAP_RIGHTS_VERSION_00);

	va_start(ap, rights);
	cap_rights_vset(rights, ap);
	va_end(ap);
}

void
_cap_rights_clear(cap_rights_t *rights, ...)
{
	va_list ap;

	assert(CAPVER(rights) == CAP_RIGHTS_VERSION_00);

	va_start(ap, rights);
	cap_rights_vclear(rights, ap);
	va_end(ap);
}

bool
_cap_rights_is_set(const cap_rights_t *rights, ...)
{
	va_list ap;
	bool ret;

	assert(CAPVER(rights) == CAP_RIGHTS_VERSION_00);

	va_start(ap, rights);
	ret = cap_rights_is_vset(rights, ap);
	va_end(ap);

	return (ret);
}

bool
cap_rights_is_valid(const cap_rights_t *rights)
{
	cap_rights_t allrights;
	unsigned int i, j;

	if (CAPVER(rights) != CAP_RIGHTS_VERSION_00)
		return (false);
	CAP_SET_ALL(&allrights);
	if (!cap_rights_contains(&allrights, rights))
		return (false);
	for (i = 0; i < CAPARSIZE(rights); i++) {
		j = right_to_index(rights->cr_rights[i]);
		if (i != j)
			return (false);
		if (i > 0) {
			if (CAPRVER(rights->cr_rights[i]) != 0)
				return (false);
		}
	}

	return (true);
}

void
cap_rights_merge(cap_rights_t *dst, const cap_rights_t *src)
{
	unsigned int i, n;

	assert(CAPVER(dst) == CAP_RIGHTS_VERSION_00);
	assert(CAPVER(src) == CAP_RIGHTS_VERSION_00);
	assert(CAPVER(dst) == CAPVER(src));
	assert(cap_rights_is_valid(src));
	assert(cap_rights_is_valid(dst));

	n = CAPARSIZE(dst);

	for (i = 0; i < n; i++)
		dst->cr_rights[i] |= src->cr_rights[i];

	assert(cap_rights_is_valid(src));
	assert(cap_rights_is_valid(dst));
}

void
cap_rights_remove(cap_rights_t *dst, const cap_rights_t *src)
{
	unsigned int i, n;

	assert(CAPVER(dst) == CAP_RIGHTS_VERSION_00);
	assert(CAPVER(src) == CAP_RIGHTS_VERSION_00);
	assert(CAPVER(dst) == CAPVER(src));
	assert(cap_rights_is_valid(src));
	assert(cap_rights_is_valid(dst));

	n = CAPARSIZE(dst);

	for (i = 0; i < n; i++) {
		dst->cr_rights[i] &=
		    ~(src->cr_rights[i] & 0x01FFFFFFFFFFFFFFULL);
	}

	assert(cap_rights_is_valid(src));
	assert(cap_rights_is_valid(dst));
}

bool
cap_rights_contains(const cap_rights_t *big, const cap_rights_t *little)
{
	unsigned int i, n;

	assert(CAPVER(big) == CAP_RIGHTS_VERSION_00);
	assert(CAPVER(little) == CAP_RIGHTS_VERSION_00);
	assert(CAPVER(big) == CAPVER(little));

	n = CAPARSIZE(big);

	for (i = 0; i < n; i++) {
		if ((big->cr_rights[i] & little->cr_rights[i]) !=
		    little->cr_rights[i]) {
			return (false);
		}
	}

	return (true);
}

#endif /* __linux__ */
