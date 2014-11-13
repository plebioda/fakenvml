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
 * obj_kvstore.c -- key-value store unit test for pmemobj
 */
#include <stdio.h>
#include <stdint.h>
#include <libpmem.h>
#include "unittest.h"
#include <string.h>

#define	DEFAULT_BUCKETS	2
#define	HASH_INIT	5381
#define	MAX_LIST	2

struct pmem_kvs_item
{
	PMEMoid next;
	PMEMoid key;
	PMEMoid val;
};

struct pmem_kvs_bucket
{
	PMEMoid head;
	PMEMmutex mutex;
	uint32_t count;
};

struct pmem_kvs_root
{
	PMEMrwlock rwlock;
	uint32_t nbuckets;
	PMEMoid buckets;
};

struct pmem_kvs
{
	PMEMobjpool *pop;
	struct pmem_kvs_root *rootp;
};

typedef struct pmem_kvs PMEMkvs;

PMEMkvs *
pmemkvs_open(char *fname)
{
	PMEMkvs *kvsp = malloc(sizeof (*kvsp));
	if (!kvsp)
		return NULL;

	kvsp->pop = pmemobj_pool_open(fname);
	if (!kvsp->pop) {
		free(kvsp);
		return NULL;
	}

	kvsp->rootp = pmemobj_root_direct(kvsp->pop, sizeof (*kvsp->rootp));
	if (!kvsp->rootp) {
		free(kvsp);
		return NULL;
	}

	jmp_buf env;
	if (setjmp(env)) {
		perror("abort");
		free(kvsp);
		return NULL;
	}

	pmemobj_tx_begin_wrlock(kvsp->pop, env, &kvsp->rootp->rwlock);

	int nbuckets = DEFAULT_BUCKETS;
	PMEMOBJ_SET(kvsp->rootp->nbuckets, nbuckets);
	kvsp->rootp->buckets = pmemobj_zalloc(
			nbuckets * sizeof (struct pmem_kvs_bucket));

	pmemobj_tx_commit();

	return kvsp;
}

void
pmemkvs_close(PMEMkvs *kvsp)
{
	pmemobj_pool_close(kvsp->pop);
	free(kvsp);
}

static uint32_t
pmemkvs_hash(PMEMkvs *kvsp, char *s)
{
	uint32_t ret = HASH_INIT;
	int c;
	while ((c = *s++))
		ret = c + ret + (ret<<5);

	pmemobj_rwlock_rdlock(&kvsp->rootp->rwlock);

	ret = ret % kvsp->rootp->nbuckets;

	pmemobj_rwlock_unlock(&kvsp->rootp->rwlock);

	return ret;
}


int
pmemkvs_bucket_insert_item(PMEMkvs *kvsp, struct pmem_kvs_bucket *bucketp,
		PMEMoid item)
{
	jmp_buf env;
	if (setjmp(env)) {
		perror("abort");
		return -1;
	}

	pmemobj_tx_begin_lock(kvsp->pop, env, &bucketp->mutex);

	struct pmem_kvs_item *itemp = pmemobj_direct(item);

	itemp->next = bucketp->head;

	PMEMOBJ_SET(bucketp->head, item);

	uint32_t new_cnt = bucketp->count + 1;
	PMEMOBJ_SET(bucketp->count, new_cnt);

	pmemobj_tx_commit();

	return 0;
}

struct pmem_kvs_item *
pmemkvs_bucket_find_key(PMEMkvs *kvsp, struct pmem_kvs_bucket *bucketp,
		char *keyp)
{
	PMEMoid curr = bucketp->head;

	while (!pmemobj_nulloid(curr)) {
		struct pmem_kvs_item *itemp = pmemobj_direct(curr);

		if (strcmp(pmemobj_direct(itemp->key), keyp) == 0)
			return itemp;

		curr = itemp->next;
	}

	return NULL;
}

int
pmemkvs_bucket_insert_kv(PMEMkvs *kvsp, struct pmem_kvs_bucket *bucketp,
		char *keyp, char *valp)
{
	jmp_buf env;
	if (setjmp(env)) {
		perror("abort");
		return -1;
	}

	pmemobj_tx_begin_lock(kvsp->pop, env, &bucketp->mutex);

	struct pmem_kvs_item *itemp = pmemkvs_bucket_find_key(kvsp,
			bucketp, keyp);

	if (NULL != itemp) {
		OUT("hash[%s]: %s -> %s", keyp,
				(char *)pmemobj_direct(itemp->val), valp);
		pmemobj_free(itemp->val);
		size_t size_val = strlen(valp) + 1;
		itemp->val = pmemobj_zalloc(size_val);
		pmemobj_memcpy(pmemobj_direct(itemp->val), valp, size_val);
	} else {
		PMEMoid item = pmemobj_zalloc(sizeof (struct pmem_kvs_item));

		itemp = pmemobj_direct(item);

		size_t size_key = strlen(keyp) + 1;
		size_t size_val = strlen(valp) + 1;

		itemp->key = pmemobj_alloc(size_key);
		itemp->val = pmemobj_alloc(size_val);

		char *kvs_keyp = pmemobj_direct(itemp->key);
		char *kvs_valp = pmemobj_direct(itemp->val);

		pmemobj_memcpy(kvs_keyp, keyp, size_key);
		pmemobj_memcpy(kvs_valp, valp, size_val);

		pmemkvs_bucket_insert_item(kvsp, bucketp, item);
	}

	pmemobj_tx_commit();

	return 0;
}

void
pmemkvs_rehash_bucket(PMEMkvs *kvsp, struct pmem_kvs_bucket *bucketp)
{
	PMEMoid curr = bucketp->head;
	PMEMoid next = {0, 0};
	while (!pmemobj_nulloid(curr)) {
		struct pmem_kvs_item *itemp = pmemobj_direct(curr);
		next = itemp->next;

		uint32_t hash = pmemkvs_hash(kvsp, pmemobj_direct(itemp->key));
		struct pmem_kvs_bucket *bucketp =
			pmemobj_direct(kvsp->rootp->buckets);

		if (pmemkvs_bucket_insert_item(kvsp, &bucketp[hash], curr))
			return;

		curr = next;
	}
}

int
pmemkvs_rehash(PMEMkvs *kvsp)
{
	jmp_buf env;
	if (setjmp(env)) {
		perror("abort");
		return -1;
	}

	pmemobj_tx_begin_wrlock(kvsp->pop, env, &kvsp->rootp->rwlock);

	uint32_t old_nbuckets = kvsp->rootp->nbuckets;
	uint32_t new_nbuckets = kvsp->rootp->nbuckets * 2;

	OUT("rehashing from %u to %u", old_nbuckets, new_nbuckets);

	PMEMOBJ_SET(kvsp->rootp->nbuckets, new_nbuckets);

	PMEMoid old_buckets = kvsp->rootp->buckets;
	PMEMoid new_buckets = pmemobj_zalloc(new_nbuckets *
			sizeof (struct pmem_kvs_bucket));
	PMEMOBJ_SET(kvsp->rootp->buckets, new_buckets);

	struct pmem_kvs_bucket *bucketp = pmemobj_direct(old_buckets);

	uint32_t i;

	for (i = 0; i < old_nbuckets; i++)
		pmemkvs_rehash_bucket(kvsp, &bucketp[i]);

	pmemobj_free(old_buckets);

	pmemobj_tx_commit();

	return 0;
}

int
pmemkvs_add(PMEMkvs *kvsp, char *keyp, char *valp)
{
	int ret = 0;

	uint32_t hash = pmemkvs_hash(kvsp, keyp);

	struct pmem_kvs_bucket *bucketp = pmemobj_direct(kvsp->rootp->buckets);

	ret = pmemkvs_bucket_insert_kv(kvsp, &bucketp[hash], keyp, valp);

	int rehash = 0;

	pmemobj_mutex_lock(&bucketp[hash].mutex);

	if (bucketp[hash].count > MAX_LIST)
		rehash = 1;

	pmemobj_mutex_unlock(&bucketp[hash].mutex);

	if (rehash)
		return pmemkvs_rehash(kvsp);
	else
		return ret;

}

const char *
pmemkvs_bucket_get_item(PMEMkvs *kvsp, struct pmem_kvs_bucket *bucketp,
		char *keyp)
{
	char *ret = NULL;
	pmemobj_mutex_lock(&bucketp->mutex);

	if (pmemobj_nulloid(bucketp->head))
		goto unlock;

	PMEMoid cur = bucketp->head;

	while (!pmemobj_nulloid(cur)) {
		struct pmem_kvs_item *itemp = pmemobj_direct(cur);
		char *kvs_keyp = pmemobj_direct(itemp->key);
		if (strcmp(kvs_keyp, keyp) == 0) {
			ret = pmemobj_direct(itemp->val);
			goto unlock;
		}

		cur = itemp->next;
	}

unlock:
	pmemobj_mutex_unlock(&bucketp->mutex);
	return ret;
}

int
pmemkvs_bucket_delete_item(PMEMkvs *kvsp, struct pmem_kvs_bucket *bucketp,
		char *keyp)
{
	jmp_buf env;
	if (setjmp(env)) {
		perror("abort");
		return -1;
	}

	pmemobj_tx_begin_lock(kvsp->pop, env, &bucketp->mutex);

	if (bucketp->count == 0)
		goto unlock;

	PMEMoid cur = bucketp->head;
	PMEMoid prev = {0, 0};

	while (!pmemobj_nulloid(cur)) {

		struct pmem_kvs_item *itemp = pmemobj_direct(cur);

		char *kvs_keyp = pmemobj_direct(itemp->key);
		if (strcmp(kvs_keyp, keyp) == 0)
			break;

		prev = cur;
		cur = itemp->next;
	}

	if (pmemobj_nulloid(prev)) {
		struct pmem_kvs_item *curp = pmemobj_direct(cur);
		PMEMOBJ_SET(bucketp->head, curp->next);
		pmemobj_free(curp->key);
		pmemobj_free(curp->val);
		pmemobj_free(cur);
	} else {

		struct pmem_kvs_item *curp = pmemobj_direct(cur);
		struct pmem_kvs_item *prevp = pmemobj_direct(prev);
		PMEMOBJ_SET(prevp->next, curp->next);
		pmemobj_free(curp->key);
		pmemobj_free(curp->val);
		pmemobj_free(cur);
	}

	uint32_t new_cnt = bucketp->count - 1;
	PMEMOBJ_SET(bucketp->count, new_cnt);

unlock:
	pmemobj_tx_commit();

	return 0;
}

const char *
pmemkvs_read(PMEMkvs *kvsp, char *keyp)
{
	uint32_t hash = pmemkvs_hash(kvsp, keyp);

	struct pmem_kvs_bucket *bucketp = pmemobj_direct(kvsp->rootp->buckets);

	return pmemkvs_bucket_get_item(kvsp, &bucketp[hash], keyp);
}

int
pmemkvs_delete(PMEMkvs *kvsp, char *keyp)
{
	uint32_t hash = pmemkvs_hash(kvsp, keyp);

	struct pmem_kvs_bucket *bucketp = pmemobj_direct(kvsp->rootp->buckets);

	return pmemkvs_bucket_delete_item(kvsp, &bucketp[hash], keyp);
}

#define	PRINT(kvsp, key)				\
do							\
{							\
	const char *valp = pmemkvs_read(kvsp, key);	\
	OUT("hash[%s] = %s", key, valp);		\
} while (0)

int
main(int argc, char *argv[])
{
	START(argc, argv, "obj_kvstore");

	if (argc < 2)
		FATAL("usage: %s file", argv[0]);

	PMEMkvs *kvsp = pmemkvs_open(argv[1]);

	pmemkvs_add(kvsp, "key1", "value1");
	pmemkvs_add(kvsp, "key2", "value2");
	pmemkvs_add(kvsp, "key3", "value3");
	pmemkvs_add(kvsp, "key4", "value4");
	pmemkvs_add(kvsp, "key5", "value5");
	pmemkvs_add(kvsp, "key6", "value6");
	pmemkvs_add(kvsp, "key7", "value7");
	pmemkvs_add(kvsp, "key8", "value8");
	pmemkvs_add(kvsp, "key9", "value9");
	pmemkvs_add(kvsp, "key1", "VALUE1");
	pmemkvs_add(kvsp, "keyA", "valueA");

	pmemkvs_delete(kvsp, "key2");
	pmemkvs_delete(kvsp, "key6");

	PRINT(kvsp, "key3");
	PRINT(kvsp, "key2");
	PRINT(kvsp, "key1");
	PRINT(kvsp, "key9");
	PRINT(kvsp, "key8");
	PRINT(kvsp, "key0");

	pmemkvs_close(kvsp);

	DONE(NULL);
}
