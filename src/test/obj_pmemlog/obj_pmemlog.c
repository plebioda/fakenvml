/*
 * Copyright (c) 2014, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * obj_pmemlog.c -- unit test for pmemobj which simulates pmemlog
 */
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libpmem.h>
#include <unistd.h>
#include "unittest.h"

#define	SIZE_OFFSET	8192

struct pmemlog_log
{
	PMEMmutex mutex;
	int init;
	uint64_t start;
	uint64_t end;
	uint64_t write;
	PMEMoid data;
};

struct pmemlog
{
	PMEMobjpool *pop;
	off_t size;
	struct pmemlog_log *logp;
};

PMEMlog *
pmemlog_pool_open(char *fname)
{
	PMEMlog *plp = malloc(sizeof (*plp));
	if (!plp)
		return NULL;

	struct stat buff;
	if (stat(fname, &buff))
		return NULL;

	plp->size = buff.st_size;

	plp->pop = pmemobj_pool_open(fname);
	if (!plp->pop) {
		free(plp);
		return NULL;
	}

	plp->logp = pmemobj_root_direct(plp->pop, sizeof (*plp->logp));

	if (plp->logp->init)
		return plp;

	jmp_buf env;
	if (setjmp(env)) {
		perror("abort");
		free(plp);
		return NULL;
	}

	pmemobj_tx_begin_lock(plp->pop, env, &plp->logp->mutex);
	plp->logp->data = pmemobj_alloc(plp->size - SIZE_OFFSET);
	uint64_t start = 0;
	uint64_t end = plp->size - SIZE_OFFSET;
	int init = 1;
	PMEMOBJ_SET(plp->logp->start, start);
	PMEMOBJ_SET(plp->logp->write, start);
	PMEMOBJ_SET(plp->logp->end, end);
	PMEMOBJ_SET(plp->logp->init, init);
	pmemobj_tx_commit();

	return plp;
}

void
pmemlog_pool_close(PMEMlog *plp)
{
	pmemobj_pool_close(plp->pop);
	free(plp);
}

int
pmemlog_append(PMEMlog *plp, const void *buf, size_t count)
{
	uint8_t *datap = pmemobj_direct(plp->logp->data);

	if (count > plp->logp->end - plp->logp->write) {
		errno = ENOSPC;
		return -1;
	}

	jmp_buf env;
	if (setjmp(env)) {
		perror("abort");
		return -1;
	}

	pmemobj_tx_begin_lock(plp->pop, env, &plp->logp->mutex);

	pmemobj_memcpy(&datap[plp->logp->write], buf, count);

	uint64_t write = plp->logp->write + count;
	PMEMOBJ_SET(plp->logp->write, write);

	pmemobj_tx_commit();

	return 0;
}

int
pmemlog_appendv(PMEMlog *plp, const struct iovec *iov, int iovcnt)
{
	uint8_t *datap = pmemobj_direct(plp->logp->data);
	uint64_t write = plp->logp->write;

	int i;
	uint8_t *buf;
	uint64_t count;

	jmp_buf env;
	if (setjmp(env)) {
		perror("abort");
		return -1;
	}

	pmemobj_tx_begin_lock(plp->pop, env, &plp->logp->mutex);

	for (i = 0; i < iovcnt; ++i) {
		buf = iov[i].iov_base;
		count = iov[i].iov_len;

		if (plp->logp->end - write < count)
			pmemobj_tx_abort(ENOSPC);

		pmemobj_memcpy(&datap[write], buf, count);

		write += count;
	}

	PMEMOBJ_SET(plp->logp->write, write);

	pmemobj_tx_commit();

	return 0;
}

void
pmemlog_rewind(PMEMlog *plp)
{
	jmp_buf env;
	if (setjmp(env)) {
		perror("abort");
		return;
	}

	pmemobj_tx_begin_lock(plp->pop, env, &plp->logp->mutex);

	uint64_t write = plp->logp->start;
	PMEMOBJ_SET(plp->logp->write, write);

	pmemobj_tx_commit();
}

off_t
pmemlog_tell(PMEMlog *plp)
{
	off_t ret = 0;
	if (pmemobj_mutex_lock(&plp->logp->mutex))
		return 0;

	ret = plp->logp->write - plp->logp->start;

	if (pmemobj_mutex_unlock(&plp->logp->mutex))
		return 0;

	return ret;
}

size_t
pmemlog_nbyte(PMEMlog *plp)
{
	size_t ret = 0;
	if (pmemobj_mutex_lock(&plp->logp->mutex))
		return 0;

	ret = plp->logp->end - plp->logp->start;

	if (pmemobj_mutex_unlock(&plp->logp->mutex))
		return 0;

	return ret;
}

void
pmemlog_walk(PMEMlog *plp, size_t chunksize,
	int (*process_chunk)(const void *buf, size_t len, void *arg),
	void *arg)
{
	if (!process_chunk)
		return;

	if (pmemobj_mutex_lock(&plp->logp->mutex))
		return;

	uint8_t *datap = pmemobj_direct(plp->logp->data);
	if (chunksize) {
		uint64_t off;
		for (off = plp->logp->start; off < plp->logp->write;
				off += chunksize) {
			process_chunk(&datap[off], chunksize, arg);
		}
	} else {
		process_chunk(datap, plp->logp->write - plp->logp->start, arg);
	}

	pmemobj_mutex_unlock(&plp->logp->mutex);
}

int
process_chunk(const void *buf, size_t len, void *arg)
{
	OUT("%s", (char *)buf);
	return 0;
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "obj_pmemlog");

	if (argc < 2)
		FATAL("usage: %s file", argv[0]);

	PMEMlog *plp = pmemlog_pool_open(argv[1]);
	if (!plp) {
		perror("pmemlog_pool_open");
		exit(EXIT_FAILURE);
	}

	pmemlog_append(plp, "String1", 8);
	pmemlog_append(plp, "String2", 8);
	pmemlog_append(plp, "String3", 8);
	struct iovec vec[3] = {
		{
			.iov_base = "String4",
			.iov_len = 8,
		},
		{
			.iov_base = "String5",
			.iov_len = 8,
		},
		{
			.iov_base = "String6",
			.iov_len = 8,
		},
	};

	pmemlog_appendv(plp, vec, 3);
	OUT("tell: %lu", pmemlog_tell(plp));
	OUT("nbytes: %lu", pmemlog_nbyte(plp));

	pmemlog_walk(plp, 8, process_chunk, NULL);
	pmemlog_rewind(plp);
	pmemlog_walk(plp, 8, process_chunk, NULL);

	pmemlog_pool_close(plp);

	DONE(NULL);
}
