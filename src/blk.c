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
 * blk.c -- block memory pool entry points for libpmem
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <uuid/uuid.h>
#include <pthread.h>
#include <endian.h>
#include <libpmem.h>

#include "pmem.h"
#include "util.h"
#include "out.h"
#include "blk.h"

/*
 * blk_init -- (internal) load-time initialization for blk
 *
 * Called automatically by the run-time loader.
 */
__attribute__((constructor))
static void
blk_init(void)
{
	out_init(LOG_PREFIX, LOG_LEVEL_VAR, LOG_FILE_VAR);
	LOG(3, NULL);
	util_init();
}

/*
 * nsread -- (internal) read data from the namespace encapsulating the BTT
 *
 * This routine is provided to btt_init() to allow the btt module to
 * do I/O on the memory pool containing the BTT layout.
 */
static int
nsread(void *ns, int lane, void *buf, size_t count, off_t off)
{
	struct pmemblk *pbp = (struct pmemblk *)ns;

	LOG(13, "pbp %p lane %d count %zu off %zu", pbp, lane, count, off);

	if (off + count >= pbp->datasize) {
		LOG(1, "offset + count (%zu) past end of data area (%zu)",
				off + count, pbp->datasize - 1);
		errno = EINVAL;
		return -1;
	}

	memcpy(buf, pbp->data + off, count);

	return 0;
}

/*
 * nswrite -- (internal) write data to the namespace encapsulating the BTT
 *
 * This routine is provided to btt_init() to allow the btt module to
 * do I/O on the memory pool containing the BTT layout.
 */
static int
nswrite(void *ns, int lane, const void *buf, size_t count, off_t off)
{
	struct pmemblk *pbp = (struct pmemblk *)ns;

	LOG(13, "pbp %p lane %d count %zu off %zu", pbp, lane, count, off);

	if (off + count >= pbp->datasize) {
		LOG(1, "offset + count (%zu) past end of data area (%zu)",
				off + count, pbp->datasize - 1);
		errno = EINVAL;
		return -1;
	}

	void *dest = pbp->data + off;

#ifdef DEBUG
	/* grab debug write lock */
	if (pthread_mutex_lock(&pbp->write_lock))
		LOG(1, "!pthread_mutex_lock");
#endif

	/* unprotect the memory (debug version only) */
	RANGE_RW(dest, count);

	memcpy(dest, buf, count);

	/* protect the memory again (debug version only) */
	RANGE_RO(dest, count);

#ifdef DEBUG
	/* release debug write lock */
	if (pthread_mutex_unlock(&pbp->write_lock))
		LOG(1, "!pthread_mutex_unlock");
#endif

	libpmem_persist(pbp->is_pmem, dest, count);

	return 0;
}

/*
 * pmemblk_map_common -- (internal) map a block memory pool
 *
 * This routine does all the work, but takes a rdonly flag so internal
 * calls can map a read-only pool if required.
 *
 * Passing in bsize == 0 means a valid pool header must exist (which
 * will supply the block size).
 */
static PMEMblk *
pmemblk_map_common(int fd, size_t bsize, int rdonly)
{
	LOG(3, "fd %d bsize %zu rdonly %d", fd, bsize, rdonly);

	/* things free by "goto err" if not NULL */
	void *addr = NULL;

	struct stat stbuf;
	if (fstat(fd, &stbuf) < 0) {
		LOG(1, "!fstat");
		return NULL;
	}

	if (stbuf.st_size < PMEMBLK_MIN_POOL) {
		LOG(1, "size %zu smaller than %zu",
				stbuf.st_size, PMEMBLK_MIN_POOL);
		errno = EINVAL;
		return NULL;
	}

	if ((addr = util_map(fd, stbuf.st_size, rdonly)) == NULL)
		return NULL;	/* util_map() set errno, called LOG */

	/* check if the mapped region is located in persistent memory */
	int is_pmem = pmem_is_pmem(addr, stbuf.st_size);

	/* opaque info lives at the beginning of mapped memory pool */
	struct pmemblk *pbp = addr;

	struct pool_hdr hdr;
	memcpy(&hdr, &pbp->hdr, sizeof (hdr));

	if (util_convert_hdr(&hdr)) {
		/*
		 * valid header found
		 */
		if (strncmp(hdr.signature, BLK_HDR_SIG, POOL_HDR_SIG_LEN)) {
			LOG(1, "wrong pool type: \"%s\"", hdr.signature);

			errno = EINVAL;
			goto err;
		}

		if (hdr.major != BLK_FORMAT_MAJOR) {
			LOG(1, "blk pool version %d (library expects %d)",
				hdr.major, BLK_FORMAT_MAJOR);

			errno = EINVAL;
			goto err;
		}

		size_t hdr_bsize = le32toh(pbp->bsize);
		if (bsize && bsize != hdr_bsize) {
			LOG(1, "wrong bsize (%zu), pool created with bsize %zu",
					bsize, hdr_bsize);
			errno = EINVAL;
			goto err;
		}
		bsize = hdr_bsize;
		LOG(3, "using block size from header: %zu", bsize);

		int retval = util_feature_check(&hdr, BLK_FORMAT_INCOMPAT,
							BLK_FORMAT_RO_COMPAT,
							BLK_FORMAT_COMPAT);
		if (retval < 0)
		    goto err;
		else if (retval == 0)
		    rdonly = 1;
	} else {
		/*
		 * no valid header was found
		 */
		if (rdonly) {
			LOG(1, "read-only and no header found");
			errno = EROFS;
			goto err;
		}
		LOG(3, "creating new blk memory pool");

		struct pool_hdr *hdrp = &pbp->hdr;

		memset(hdrp, '\0', sizeof (*hdrp));
		strncpy(hdrp->signature, BLK_HDR_SIG, POOL_HDR_SIG_LEN);
		hdrp->major = htole32(BLK_FORMAT_MAJOR);
		hdrp->compat_features = htole32(BLK_FORMAT_COMPAT);
		hdrp->incompat_features = htole32(BLK_FORMAT_INCOMPAT);
		hdrp->ro_compat_features = htole32(BLK_FORMAT_RO_COMPAT);
		uuid_generate(hdrp->uuid);
		hdrp->crtime = htole64((uint64_t)time(NULL));
		util_checksum(hdrp, sizeof (*hdrp), &hdrp->checksum, 1);
		hdrp->checksum = htole64(hdrp->checksum);

		/* store pool's header */
		libpmem_persist(is_pmem, hdrp, sizeof (*hdrp));

		/* create rest of required metadata */
		pbp->bsize = htole32(bsize);
		libpmem_persist(is_pmem, &pbp->bsize, sizeof (bsize));
	}

	/*
	 * Use some of the memory pool area for run-time info.  This
	 * run-time state is never loaded from the file, it is always
	 * created here, so no need to worry about byte-order.
	 */
	pbp->addr = addr;
	pbp->size = stbuf.st_size;
	pbp->rdonly = rdonly;
	pbp->is_pmem = is_pmem;
	pbp->data = addr + roundup(sizeof (*pbp), BLK_FORMAT_DATA_ALIGN);
	pbp->datasize = (pbp->addr + pbp->size) - pbp->data;

	LOG(4, "data area %p data size %zu bsize %zu",
		pbp->data, pbp->datasize, bsize);

	int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus < 1)
		ncpus = 1;

#ifdef DEBUG
	/* initialize debug lock */
	if (pthread_mutex_init(&pbp->write_lock, NULL) < 0) {
		LOG(1, "!pthread_mutex_init");
		goto err;
	}
#endif

	/*
	 * If possible, turn off all permissions on the pool header page.
	 *
	 * The prototype PMFS doesn't allow this when large pages are in
	 * use not it is not considered an error if this fails.
	 */
	util_range_none(addr, sizeof (struct pool_hdr));

	/* the data area should be kept read-only for debug version */
	RANGE_RO(pbp->data, pbp->datasize);

	LOG(3, "pbp %p", pbp);
	return pbp;

err:
	LOG(4, "error clean up");
	int oerrno = errno;
	util_unmap(addr, stbuf.st_size);
	errno = oerrno;
	return NULL;
}

/*
 * pmemblk_map -- map a block memory pool
 */
PMEMblk *
pmemblk_map(int fd, size_t bsize)
{
	LOG(3, "fd %d bsize %zu", fd, bsize);

	return pmemblk_map_common(fd, bsize, 0);
}

/*
 * pmemblk_unmap -- unmap a block memory pool
 */
void
pmemblk_unmap(PMEMblk *pbp)
{
	LOG(3, "pbp %p", pbp);

#ifdef DEBUG
	/* destroy debug lock */
	pthread_mutex_destroy(&pbp->write_lock);
#endif

	util_unmap(pbp->addr, pbp->size);
}

/*
 * pmemblk_nblock -- return number of usable blocks in a block memory pool
 */
size_t
pmemblk_nblock(PMEMblk *pbp)
{
	LOG(3, "pbp %p", pbp);

	return pbp->size / pbp->bsize;
}

/*
 * pmemblk_read -- read a block in a block memory pool
 */
int
pmemblk_read(PMEMblk *pbp, void *buf, off_t blockno)
{
	LOG(3, "pbp %p buf %p blockno %zu", pbp, buf, blockno);

	return nsread(pbp, 0, buf, pbp->bsize, blockno * pbp->bsize);
}

/*
 * pmemblk_write -- write a block (atomically) in a block memory pool
 */
int
pmemblk_write(PMEMblk *pbp, const void *buf, off_t blockno)
{
	LOG(3, "pbp %p buf %p blockno %zu", pbp, buf, blockno);

	if (pbp->rdonly) {
		LOG(1, "EROFS (pool is read-only)");
		errno = EROFS;
		return -1;
	}

	return nswrite(pbp, 0, buf, pbp->bsize, blockno * pbp->bsize);
}

/*
 * pmemblk_set_zero -- zero a block in a block memory pool
 */
int
pmemblk_set_zero(PMEMblk *pbp, off_t blockno)
{
	LOG(3, "pbp %p blockno %zu", pbp, blockno);

	if (pbp->rdonly) {
		LOG(1, "EROFS (pool is read-only)");
		errno = EROFS;
		return -1;
	}

	return -1;
}

/*
 * pmemblk_set_error -- set the error state on a block in a block memory pool
 */
int
pmemblk_set_error(PMEMblk *pbp, off_t blockno)
{
	LOG(3, "pbp %p blockno %zu", pbp, blockno);

	if (pbp->rdonly) {
		LOG(1, "EROFS (pool is read-only)");
		errno = EROFS;
		return -1;
	}

	return -1;
}

/*
 * pmemblk_check -- block memory pool consistency check
 */
int
pmemblk_check(const char *path)
{
	LOG(3, "path \"%s\"", path);

	return 1;
}
