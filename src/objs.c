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
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY LOG OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * objs.c -- transactional object store implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <uuid/uuid.h>
#include <endian.h>
#include <libpmem.h>
#include "pmem.h"
#include "util.h"
#include "out.h"
#include "objs.h"

/*
 * objs_init -- load-time initialization for objs
 *
 * Called automatically by the run-time loader.
 */
__attribute__((constructor))
static void
objs_init(void)
{
	out_init(LOG_PREFIX, LOG_LEVEL_VAR, LOG_FILE_VAR);
	LOG(3, NULL);
	util_init();
}

/*
 * pmemobjs_map -- map a transactional memory pool
 */
PMEMobjs *
pmemobjs_map(int fd)
{
	LOG(3, "fd %d", fd);

	struct stat stbuf;

	if (fstat(fd, &stbuf) < 0) {
		LOG(1, "!fstat");
		return NULL;
	}

	if (stbuf.st_size < PMEMOBJS_MIN_POOL) {
		LOG(1, "size %zu smaller than %zu",
				stbuf.st_size, PMEMOBJS_MIN_POOL);
		errno = EINVAL;
		return NULL;
	}

	void *addr;
	if ((addr = util_map(fd, stbuf.st_size, 0)) == NULL)
		return NULL;	/* util_map() set errno, called LOG */

	/* check if the mapped region is located in persistent memory */
	int is_pmem = pmem_is_pmem(addr, stbuf.st_size);

	/* opaque info lives at the beginning of mapped memory pool */
	struct pmemobjs *pop = addr;

	struct pool_hdr hdr;
	memcpy(&hdr, &pop->hdr, sizeof (hdr));

	if (util_convert_hdr(&hdr)) {
		/*
		 * valid header found
		 */
		if (strncmp(hdr.signature, OBJS_HDR_SIG, POOL_HDR_SIG_LEN)) {
			LOG(1, "wrong pool type: \"%s\"", hdr.signature);

			errno = EINVAL;
			goto err;
		}

		if (hdr.major != OBJS_FORMAT_MAJOR) {
			LOG(1, "objs pool version %d (library expects %d)",
				hdr.major, OBJS_FORMAT_MAJOR);

			errno = EINVAL;
			goto err;
		}

		int retval = util_feature_check(&hdr, OBJS_FORMAT_INCOMPAT,
							OBJS_FORMAT_RO_COMPAT,
							OBJS_FORMAT_COMPAT);
		if (retval < 0)
		    goto err;
		else if (retval == 0) {
			/* XXX switch to read-only mode */
		}
	} else {
		/*
		 * no valid header was found
		 */
		LOG(3, "creating new objs memory pool");

		struct pool_hdr *hdrp = &pop->hdr;

		memset(hdrp, '\0', sizeof (*hdrp));
		strncpy(hdrp->signature, OBJS_HDR_SIG, POOL_HDR_SIG_LEN);
		hdrp->major = htole32(OBJS_FORMAT_MAJOR);
		hdrp->compat_features = htole32(OBJS_FORMAT_COMPAT);
		hdrp->incompat_features = htole32(OBJS_FORMAT_INCOMPAT);
		hdrp->ro_compat_features = htole32(OBJS_FORMAT_RO_COMPAT);
		uuid_generate(hdrp->uuid);
		hdrp->crtime = htole64((uint64_t)time(NULL));
		util_checksum(hdrp, sizeof (*hdrp), &hdrp->checksum, 1);
		hdrp->checksum = htole64(hdrp->checksum);

		/* store pool's header */
		libpmem_persist(is_pmem, hdrp, sizeof (*hdrp));

		/* XXX create rest of required metadata */
	}

	/* use some of the memory pool area for run-time info */
	pop->addr = addr;
	pop->size = stbuf.st_size;

	/*
	 * If possible, turn off all permissions on the pool header page.
	 *
	 * The prototype PMFS doesn't allow this when large pages are in
	 * use not it is not considered an error if this fails.
	 */
	util_range_none(addr, sizeof (struct pool_hdr));

	/* the rest should be kept read-only for debug version */
	RANGE_RO(addr + sizeof (struct pool_hdr),
			stbuf.st_size - sizeof (struct pool_hdr));

	LOG(3, "pop %p", pop);
	return pop;

err:
	LOG(4, "error clean up");
	int oerrno = errno;
	util_unmap(addr, stbuf.st_size);
	errno = oerrno;
	return NULL;
}

/*
 * pmemobjs_unmap -- unmap a transactional memory pool
 */
void
pmemobjs_unmap(PMEMobjs *pop)
{
	LOG(3, "pop %p", pop);

	util_unmap(pop->addr, pop->size);
}

/*
 * pmemobjs_check -- transactional memory pool consistency check
 */
int
pmemobjs_check(const char *path)
{
	LOG(3, "path \"%s\"", path);

	/* XXX stub */
	return 0;
}

/*
 * pmemobjs_mutex_lock -- lock a PMEMmutex
 */
int
pmemobjs_mutex_lock(PMEMmutex *mutexp)
{
	return 0;
}

/*
 * pmemobjs_mutex_unlock -- unlock a PMEMmutex
 */
int
pmemobjs_mutex_unlock(PMEMmutex *mutexp)
{
	return 0;
}

/*
 * pmemobjs_rwlock_rdlock -- read lock a PMEMrwlock
 */
int
pmemobjs_rwlock_rdlock(PMEMrwlock *rwlockp)
{
	return 0;
}

/*
 * pmemobjs_rwlock_wrlock -- write lock a PMEMrwlock
 */
int
pmemobjs_rwlock_wrlock(PMEMrwlock *rwlockp)
{
	return 0;
}

/*
 * pmemobjs_rwlock_unlock -- unlock a PMEMrwlock
 */
int
pmemobjs_rwlock_unlock(PMEMrwlock *rwlockp)
{
	return 0;
}

/*
 * pmemobjs_root -- return root object ID
 */
PMEMoid
pmemobjs_root(PMEMobjs *pop, size_t size)
{
	PMEMoid r = { 0 };

	return r;
}

/*
 * pmemobjs_root_direct -- return direct access to root object
 *
 * The root object is special.  If it doesn't exist, a pre-zeroed instance
 * is created, persisted, and then returned.  If it does exist, the
 * instance already in pmem is returned.  Creation is done atomically, so
 * two threads calling pmemobjs_root_direct() concurrently will get back
 * the same pointer to the same object, even if it has to be created.  But
 * beyond that there's no protection against concurrent updates and the
 * object almost certainly needs to contain a lock to make updates to it
 * MT-safe.
 *
 * The argument "size" is used to determine the size of the root object,
 * the first time this is called, but after that the object already exists
 * and size is used to verify the caller knows the correct size.
 */
void *
pmemobjs_root_direct(PMEMobjs *pop, size_t size)
{
	return pmemobjs_direct(pmemobjs_root(pop, size));
}

/*
 * pmemobjs_root_resize -- set the root object size
 *
 * This is for the (extremely rare) case where the root object needs
 * to change size.  If the object grows in size, the new portion of
 * the object is zeroed.
 */
int
pmemobjs_root_resize(PMEMobjs *pop, size_t newsize)
{
	return 0;
}

/*
 * pmemobjs_begin -- begin a transaction
 */
int
pmemobjs_begin(PMEMobjs *pop, jmp_buf env)
{
	return 0;
}

/*
 * pmemobjs_begin -- begin a transaction with a mutex
 */
int
pmemobjs_begin_mutex(PMEMobjs *pop, jmp_buf env, PMEMmutex *mutexp)
{
	return 0;
}

/*
 * pmemobjs_commit -- commit transaction, implicit tid
 */
int
pmemobjs_commit(void)
{
	return 0;
}

/*
 * pmemobjs_abort -- abort transaction, implicit tid
 */
int
pmemobjs_abort(int errnum)
{
	return 0;
}

/*
 * pmemobjs_alloc -- transactional allocate, implicit tid
 */
PMEMoid
pmemobjs_alloc(size_t size)
{
	PMEMoid n = { 0 };

	return n;
}

/*
 * pmemobjs_zalloc -- transactional allocate, zeroed, implicit tid
 */
PMEMoid
pmemobjs_zalloc(size_t size)
{
	PMEMoid n = { 0 };

	return n;
}

/*
 * pmemobjs_free -- transactional free, implicit tid
 */
int
pmemobjs_free(PMEMoid oid)
{
	return 0;
}

/*
 * pmemobjs_direct -- return direct access to an object
 *
 * The direct access is for fetches only, stores must be done by
 * pmemobjs_memcpy() or PMEMOBJS_SET().  When debugging is enabled,
 * attempting to store to the pointer returned by this call will
 * result in a SEGV.
 */
void *
pmemobjs_direct(PMEMoid oid)
{
	return NULL;
}

/*
 * pmemobjs_direct_ntx -- return direct access to an object, non-transactional
 */
void *
pmemobjs_direct_ntx(PMEMoid oid)
{
	return NULL;
}

/*
 * pmemobjs_nulloid -- true is object ID is the NULL object
 */
int
pmemobjs_nulloid(PMEMoid oid)
{
	return 0;
}

/*
 * pmemobjs_memcpy -- change a range of pmem, making undo log entries as well
 */
int
pmemobjs_memcpy(void *dstp, void *srcp, size_t size)
{
	return 0;
}
