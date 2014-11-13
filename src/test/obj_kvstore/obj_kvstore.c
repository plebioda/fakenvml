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

struct kvs_item
{
	PMEMoid next;
	PMEMoid key;
	PMEMoid val;
};

struct pmem_kvs_bucket
{
	PMEMoid head;
	PMEMmutex mutex;
};

struct pmem_kvs_root
{
	PMEMmutex mutex;
	int nbuckets;
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

	pmemobj_tx_begin_lock(kvsp->pop, env, &kvsp->rootp->mutex);

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
pmemkvs_hash(char *s)
{
	uint32_t ret = HASH_INIT;
	int c;
	while ((c = *s++))
		ret = c + ret + (ret<<5);

	return ret;
}

int
pmemkvs_insert(PMEMkvs *kvsp, struct pmem_kvs_bucket *bucketp,
		char *keyp, char *valp)
{
	jmp_buf env;
	if (setjmp(env)) {
		perror("abort");
		free(kvsp);
		return -1;
	}

	pmemobj_tx_begin_lock(kvsp->pop, env, &bucketp->mutex);

	PMEMoid item = pmemobj_zalloc(sizeof (struct kvs_item));

	struct kvs_item *itemp = pmemobj_direct(item);

	size_t size_key = strlen(keyp) + 1;
	size_t size_val = strlen(valp) + 1;

	itemp->key = pmemobj_alloc(size_key);
	itemp->val = pmemobj_alloc(size_val);

	char *kvs_keyp = pmemobj_direct(itemp->key);
	char *kvs_valp = pmemobj_direct(itemp->val);

	pmemobj_memcpy(kvs_keyp, keyp, size_key);
	pmemobj_memcpy(kvs_valp, valp, size_val);

	itemp->next = bucketp->head;

	PMEMOBJ_SET(bucketp->head, item);

	pmemobj_tx_commit();

	return 0;
}

int
pmemkvs_add(PMEMkvs *kvsp, char *keyp, char *valp)
{
	int ret = 0;

	uint32_t hash = pmemkvs_hash(keyp);

	hash = hash % kvsp->rootp->nbuckets;

	struct pmem_kvs_bucket *bucketp = pmemobj_direct(kvsp->rootp->buckets);

	ret = pmemkvs_insert(kvsp, &bucketp[hash], keyp, valp);

	return ret;
}

const char *
pmemkvs_get_item(PMEMkvs *kvsp, struct pmem_kvs_bucket *bucketp, char *keyp)
{
	char *ret = NULL;
	pmemobj_mutex_lock(&bucketp->mutex);

	if (pmemobj_nulloid(bucketp->head))
		goto unlock;

	PMEMoid cur = bucketp->head;

	while (!pmemobj_nulloid(cur)) {
		struct kvs_item *itemp = pmemobj_direct(cur);
		char *kvs_keyp = pmemobj_direct(itemp->key);
		if (!strcmp(kvs_keyp, keyp)) {
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
pmemkvs_delete_item(PMEMkvs *kvsp, struct pmem_kvs_bucket *bucketp, char *keyp)
{
	jmp_buf env;
	if (setjmp(env)) {
		perror("abort");
		free(kvsp);
		return -1;
	}

	pmemobj_tx_begin_lock(kvsp->pop, env, &bucketp->mutex);

	PMEMoid cur = bucketp->head;
	PMEMoid prev = {0, 0};
	while (!pmemobj_nulloid(cur)) {

		struct kvs_item *itemp = pmemobj_direct(cur);

		char *kvs_keyp = pmemobj_direct(itemp->key);
		if (!strcmp(kvs_keyp, keyp))
			break;

		prev = cur;
		cur = itemp->next;
	}

	if (pmemobj_nulloid(prev)) {
		struct kvs_item *curp = pmemobj_direct(cur);
		PMEMOBJ_SET(bucketp->head, curp->next);
		pmemobj_free(curp->key);
		pmemobj_free(curp->val);
		pmemobj_free(cur);
	} else {

		struct kvs_item *curp = pmemobj_direct(cur);
		struct kvs_item *prevp = pmemobj_direct(prev);
		PMEMOBJ_SET(prevp->next, curp->next);
		pmemobj_free(curp->key);
		pmemobj_free(curp->val);
		pmemobj_free(cur);
	}

	pmemobj_tx_commit();

	return 0;
}

const char *
pmemkvs_read(PMEMkvs *kvsp, char *keyp)
{
	uint32_t hash = pmemkvs_hash(keyp);

	hash = hash % kvsp->rootp->nbuckets;

	struct pmem_kvs_bucket *bucketp = pmemobj_direct(kvsp->rootp->buckets);

	return pmemkvs_get_item(kvsp, &bucketp[hash], keyp);
}

int
pmemkvs_delete(PMEMkvs *kvsp, char *keyp)
{

	uint32_t hash = pmemkvs_hash(keyp);

	hash = hash % kvsp->rootp->nbuckets;

	struct pmem_kvs_bucket *bucketp = pmemobj_direct(kvsp->rootp->buckets);

	return pmemkvs_delete_item(kvsp, &bucketp[hash], keyp);
}

#define	PRINT(kvsp, key)				\
do							\
{							\
	const char *valp = pmemkvs_read(kvsp, key);	\
	OUT("hash[%s] = %s", key, valp);		\
} while(0)

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
