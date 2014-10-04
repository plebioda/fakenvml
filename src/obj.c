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
 * obj.c -- transactional object store implementation
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
#include "obj.h"

/*
 * obj_init -- load-time initialization for obj
 *
 * Called automatically by the run-time loader.
 */
__attribute__((constructor))
static void
obj_init(void)
{
	out_init(LOG_PREFIX, LOG_LEVEL_VAR, LOG_FILE_VAR);
	LOG(3, NULL);
	util_init();
}

/*
 * pmemobj_pool_open -- open a transactional memory pool
 */
PMEMobjpool *
pmemobj_pool_open(const char *path)
{
	LOG(3, "path \"%s\"", path);

	int fd;
	if ((fd = open(path, O_RDWR))< 0) {
		LOG(1, "!%s", path);
		return NULL;
	}

	struct stat stbuf;

	if (fstat(fd, &stbuf) < 0) {
		LOG(1, "!fstat");
		close(fd);
		return NULL;
	}

	if (stbuf.st_size < PMEMOBJ_MIN_POOL) {
		LOG(1, "size %zu smaller than %zu",
				stbuf.st_size, PMEMOBJ_MIN_POOL);
		errno = EINVAL;
		close(fd);
		return NULL;
	}

	void *addr;
	if ((addr = util_map(fd, stbuf.st_size, 0)) == NULL)
		return NULL;	/* util_map() set errno, called LOG */

	close(fd);

	/* check if the mapped region is located in persistent memory */
	int is_pmem = pmem_is_pmem(addr, stbuf.st_size);

	/* opaque info lives at the beginning of mapped memory pool */
	struct pmemobjpool *pop = addr;

	struct pool_hdr hdr;
	memcpy(&hdr, &pop->hdr, sizeof (hdr));

	if (util_convert_hdr(&hdr)) {
		/*
		 * valid header found
		 */
		if (strncmp(hdr.signature, OBJ_HDR_SIG, POOL_HDR_SIG_LEN)) {
			LOG(1, "wrong pool type: \"%s\"", hdr.signature);

			errno = EINVAL;
			goto err;
		}

		if (hdr.major != OBJ_FORMAT_MAJOR) {
			LOG(1, "obj pool version %d (library expects %d)",
				hdr.major, OBJ_FORMAT_MAJOR);

			errno = EINVAL;
			goto err;
		}

		int retval = util_feature_check(&hdr, OBJ_FORMAT_INCOMPAT,
							OBJ_FORMAT_RO_COMPAT,
							OBJ_FORMAT_COMPAT);
		if (retval < 0)
		    goto err;
		else if (retval == 0) {
			/* XXX switch to read-only mode */
		}
	} else {
		/*
		 * no valid header was found
		 */
		LOG(3, "creating new obj memory pool");

		struct pool_hdr *hdrp = &pop->hdr;

		memset(hdrp, '\0', sizeof (*hdrp));
		strncpy(hdrp->signature, OBJ_HDR_SIG, POOL_HDR_SIG_LEN);
		hdrp->major = htole32(OBJ_FORMAT_MAJOR);
		hdrp->compat_features = htole32(OBJ_FORMAT_COMPAT);
		hdrp->incompat_features = htole32(OBJ_FORMAT_INCOMPAT);
		hdrp->ro_compat_features = htole32(OBJ_FORMAT_RO_COMPAT);
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
 * pmemobj_pool_open_mirrored -- open a mirrored pool
 */
PMEMobjpool *
pmemobj_pool_open_mirrored(const char *path1, const char *path2)
{
	return NULL;
}

/*
 * pmemobj_pool_close -- close a transactional memory pool
 */
void
pmemobj_pool_close(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	util_unmap(pop->addr, pop->size);
}

/*
 * pmemobj_pool_check -- transactional memory pool consistency check
 */
int
pmemobj_pool_check(const char *path)
{
	LOG(3, "path \"%s\"", path);

	/* XXX stub */
	return 0;
}

/*
 * pmemobj_pool_check_mirrored -- mirrored memory pool consistency check
 */
int
pmemobj_pool_check_mirrored(const char *path1, const char *path2)
{
	LOG(3, "path1 \"%s\", path2 \"%s\"", path1, path2);

	/* XXX stub */
	return 0;
}

/*
 * pmemobj_mutex_init -- initialize a PMEMmutex
 */
int
pmemobj_mutex_init(PMEMmutex *mutexp)
{
	return 0;
}

/*
 * pmemobj_mutex_lock -- lock a PMEMmutex
 */
int
pmemobj_mutex_lock(PMEMmutex *mutexp)
{
	return 0;
}

/*
 * pmemobj_mutex_unlock -- unlock a PMEMmutex
 */
int
pmemobj_mutex_unlock(PMEMmutex *mutexp)
{
	return 0;
}

/*
 * pmemobj_rwlock_init -- initialize a PMEMrwlock
 */
int
pmemobj_rwlock_init(PMEMrwlock *rwlockp)
{
	return 0;
}

/*
 * pmemobj_rwlock_rdlock -- read lock a PMEMrwlock
 */
int
pmemobj_rwlock_rdlock(PMEMrwlock *rwlockp)
{
	return 0;
}

/*
 * pmemobj_rwlock_wrlock -- write lock a PMEMrwlock
 */
int
pmemobj_rwlock_wrlock(PMEMrwlock *rwlockp)
{
	return 0;
}

/*
 * pmemobj_rwlock_unlock -- unlock a PMEMrwlock
 */
int
pmemobj_rwlock_timedrdlock(PMEMrwlock *restrict rwlockp,
		const struct timespec *restrict abs_timeout)
{
	return 0;
}

/*
 * pmemobj_rwlock_unlock -- unlock a PMEMrwlock
 */
int
pmemobj_rwlock_timedwrlock(PMEMrwlock *restrict rwlockp,
		const struct timespec *restrict abs_timeout)
{
	return 0;
}

/*
 * pmemobj_rwlock_unlock -- unlock a PMEMrwlock
 */
int
pmemobj_rwlock_tryrdlock(PMEMrwlock *rwlockp)
{
	return 0;
}

/*
 * pmemobj_rwlock_unlock -- unlock a PMEMrwlock
 */
int
pmemobj_rwlock_trywrlock(PMEMrwlock *rwlockp)
{
	return 0;
}

/*
 * pmemobj_rwlock_unlock -- unlock a PMEMrwlock
 */
int
pmemobj_rwlock_unlock(PMEMrwlock *rwlockp)
{
	return 0;
}

/*
 * pmemobj_cond_init -- initialize a PMEMcond
 */
int
pmemobj_cond_init(PMEMcond *condp)
{
	return 0;
}

/*
 * pmemobj_cond_init -- initialize a PMEMcond
 */
int
pmemobj_cond_broadcast(PMEMcond *condp)
{
	return 0;
}

/*
 * pmemobj_cond_init -- initialize a PMEMcond
 */
int
pmemobj_cond_signal(PMEMcond *condp)
{
	return 0;
}

/*
 * pmemobj_cond_init -- initialize a PMEMcond
 */
int
pmemobj_cond_timedwait(PMEMcond *restrict condp,
		PMEMmutex *restrict mutexp,
		const struct timespec *restrict abstime)
{
	return 0;
}

/*
 * pmemobj_cond_init -- initialize a PMEMcond
 */
int
pmemobj_cond_wait(PMEMcond *condp, PMEMmutex *restrict mutexp)
{
	return 0;
}

/*
 * pmemobj_root -- return root object ID
 */
PMEMoid
pmemobj_root(PMEMobjpool *pop, size_t size)
{
	PMEMoid r = { 0 };

	return r;
}

/*
 * pmemobj_root_direct -- return direct access to root object
 *
 * The root object is special.  If it doesn't exist, a pre-zeroed instance
 * is created, persisted, and then returned.  If it does exist, the
 * instance already in pmem is returned.  Creation is done atomically, so
 * two threads calling pmemobj_root_direct() concurrently will get back
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
pmemobj_root_direct(PMEMobjpool *pop, size_t size)
{
	return pmemobj_direct(pmemobj_root(pop, size));
}

/*
 * pmemobj_root_resize -- set the root object size
 *
 * This is for the (extremely rare) case where the root object needs
 * to change size.  If the object grows in size, the new portion of
 * the object is zeroed.
 */
int
pmemobj_root_resize(PMEMobjpool *pop, size_t newsize)
{
	return 0;
}

/*
 * pmemobj_tx_begin -- begin a transaction
 */
int
pmemobj_tx_begin(PMEMobjpool *pop, jmp_buf env)
{
	return 0;
}

/*
 * pmemobj_tx_begin_lock -- begin a transaction, locking a mutex
 */
int
pmemobj_tx_begin_lock(PMEMobjpool *pop, jmp_buf env, PMEMmutex *mutexp)
{
	return 0;
}

/*
 * pmemobj_tx_begin_wrlock -- begin a transaction, write locking an rwlock
 */
int
pmemobj_tx_begin_wrlock(PMEMobjpool *pop, jmp_buf env, PMEMrwlock *rwlockp)
{
	return 0;
}

/*
 * pmemobj_tx_commit -- commit transaction, implicit tid
 */
int
pmemobj_tx_commit(void)
{
	return 0;
}

/*
 * pmemobj_tx_commit_tid -- commit transaction
 */
int
pmemobj_tx_commit_tid(int tid)
{
	return 0;
}

/*
 * pmemobj_tx_commit_multi -- commit multiple transactions
 *
 * A list of tids is provided varargs-style, terminated by 0
 */
int
pmemobj_tx_commit_multi(int tid, ...)
{
	return 0;
}

/*
 * pmemobj_tx_commit_multiv -- commit multiple transactions, on array of tids
 *
 * A list of tids is provided as an array, terminated by a 0 entry
 */
int
pmemobj_tx_commit_multiv(int tids[])
{
	return 0;
}

/*
 * pmemobj_tx_abort -- abort transaction, implicit tid
 */
int
pmemobj_tx_abort(int errnum)
{
	return 0;
}

/*
 * pmemobj_tx_abort_tid -- abort transaction
 */
int
pmemobj_tx_abort_tid(int tid, int errnum)
{
	return 0;
}

/*
 * pmemobj_alloc -- transactional allocate, implicit tid
 */
PMEMoid
pmemobj_alloc(size_t size)
{
	PMEMoid n = { 0 };

	return n;
}

/*
 * pmemobj_zalloc -- transactional allocate, zeroed, implicit tid
 */
PMEMoid
pmemobj_zalloc(size_t size)
{
	PMEMoid n = { 0 };

	return n;
}

/*
 * pmemobj_realloc -- transactional realloc, implicit tid
 */
PMEMoid
pmemobj_realloc(PMEMoid oid, size_t size)
{
	PMEMoid n = { 0 };

	return n;
}

/*
 * pmemobj_aligned_alloc -- transactional alloc of aligned memory, implicit tid
 */
PMEMoid
pmemobj_aligned_alloc(size_t alignment, size_t size)
{
	PMEMoid n = { 0 };

	return n;
}

/*
 * pmemobj_strdup -- transactional strdup of non-pmem string, implicit tid
 */
PMEMoid
pmemobj_strdup(const char *s)
{
	PMEMoid n = { 0 };

	return n;
}

/*
 * pmemobj_free -- transactional free, implicit tid
 */
int
pmemobj_free(PMEMoid oid)
{
	return 0;
}

/*
 * pmemobj_alloc_tid -- transactional allocate
 */
PMEMoid
pmemobj_alloc_tid(int tid, size_t size)
{
	PMEMoid n = { 0 };

	return n;
}

/*
 * pmemobj_zalloc_tid -- transactional allocate, zeroed
 */
PMEMoid
pmemobj_zalloc_tid(int tid, size_t size)
{
	PMEMoid n = { 0 };

	return n;
}

/*
 * pmemobj_realloc_tid -- transactional realloc
 */
PMEMoid
pmemobj_realloc_tid(int tid, PMEMoid oid, size_t size)
{
	PMEMoid n = { 0 };

	return n;
}

/*
 * pmemobj_aligned_alloc -- transactional alloc of aligned memory
 */
PMEMoid
pmemobj_aligned_alloc_tid(int tid, size_t alignment, size_t size)
{
	PMEMoid n = { 0 };

	return n;
}

/*
 * pmemobj_strdup_tid -- transactional strdup of non-pmem string
 */
PMEMoid
pmemobj_strdup_tid(int tid, const char *s)
{
	PMEMoid n = { 0 };

	return n;
}

/*
 * pmemobj_free_tid -- transactional free
 */
int
pmemobj_free_tid(int tid, PMEMoid oid)
{
	return 0;
}

/*
 * pmemobj_direct -- return direct access to an object
 *
 * The direct access is for fetches only, stores must be done by
 * pmemobj_memcpy() or PMEMOBJ_SET().  When debugging is enabled,
 * attempting to store to the pointer returned by this call will
 * result in a SEGV.
 */
void *
pmemobj_direct(PMEMoid oid)
{
	return NULL;
}

/*
 * pmemobj_direct_ntx -- return direct access to an object, non-transactional
 */
void *
pmemobj_direct_ntx(PMEMoid oid)
{
	return NULL;
}

/*
 * pmemobj_nulloid -- true is object ID is the NULL object
 */
int
pmemobj_nulloid(PMEMoid oid)
{
	return 0;
}

/*
 * pmemobj_memcpy -- change a range, making undo log entries, implicit tid
 */
int
pmemobj_memcpy(void *dstp, void *srcp, size_t size)
{
	return 0;
}

/*
 * pmemobj_memcpy_tid -- change a range, making undo log entries
 */
int
pmemobj_memcpy_tid(int tid, void *dstp, void *srcp, size_t size)
{
	return 0;
}
