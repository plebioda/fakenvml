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

#include "unittest.h"
#include "libpmem.h"
#include <assert.h>

struct base {
	PMEMoid test;
	PMEMmutex mutex;
};

#define	TEST_VALUE_A 5
#define	TEST_VALUE_B 6
#define	TEST_INNER_LOOPS 2

#define	code_not_reached() assert(0)

void
do_test_alloc_single_transaction(PMEMobjpool *pop)
{
	struct base *bp = pmemobj_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env)) {
		code_not_reached();
		return;
	}

	pmemobj_tx_begin_lock(pop, env, &bp->mutex);

	bp->test = pmemobj_alloc(sizeof (int));
	int *ptr_test = pmemobj_direct(bp->test);
	*ptr_test = TEST_VALUE_A;

	pmemobj_tx_commit();

	ptr_test = pmemobj_direct(bp->test);
	assert(*ptr_test == TEST_VALUE_A);
}

void
do_test_alloc_huge_single_transaction(PMEMobjpool *pop)
{
	struct base *bp = pmemobj_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env)) {
		code_not_reached();
		return;
	}

	pmemobj_tx_begin_lock(pop, env, &bp->mutex);

	bp->test = pmemobj_alloc(20 * 1024 * 1024); /* 20MB */
	int *ptr_test = pmemobj_direct(bp->test);
	*ptr_test = TEST_VALUE_A;

	pmemobj_tx_commit();

	ptr_test = pmemobj_direct(bp->test);
	assert(*ptr_test == TEST_VALUE_A);
}

void
do_test_set_single_transaction(PMEMobjpool *pop)
{
	struct base *bp = pmemobj_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env)) {
		code_not_reached();
		return;
	}

	pmemobj_tx_begin_lock(pop, env, &bp->mutex);

	int b = TEST_VALUE_B;
	int *ptr_test = pmemobj_direct(bp->test);
	pmemobj_memcpy(ptr_test, &b, sizeof (int));
	pmemobj_tx_commit();

	ptr_test = pmemobj_direct(bp->test);
	assert(*ptr_test == TEST_VALUE_B);
}

void
do_test_delete_single_transaction(PMEMobjpool *pop)
{
	struct base *bp = pmemobj_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env)) {
		code_not_reached();
		return;
	}

	pmemobj_tx_begin_lock(pop, env, &bp->mutex);
	pmemobj_free(bp->test);
	pmemobj_tx_commit();
}

void do_test_combine_two_transactions(PMEMobjpool *pop) {
	struct base *bp = pmemobj_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env)) {
		code_not_reached();
		return;
	}

	pmemobj_tx_begin_lock(pop, env, &bp->mutex);

	PMEMoid value = pmemobj_alloc(sizeof (int));
	int *ptr_value = pmemobj_direct(value);
	*ptr_value = TEST_VALUE_A;
	PMEMOBJ_SET(bp->test, value);

	int *ptr_test = pmemobj_direct(bp->test);
	assert(*ptr_test == TEST_VALUE_A);

	value = pmemobj_alloc(sizeof (int));
	ptr_value = pmemobj_direct(value);
	int b = TEST_VALUE_B;
	pmemobj_memcpy(ptr_value, &b, sizeof (int));

	pmemobj_free(bp->test);

	PMEMOBJ_SET(bp->test, value);

	pmemobj_tx_commit();

	ptr_test = pmemobj_direct(bp->test);
	assert(*ptr_test == TEST_VALUE_B);

	pmemobj_tx_begin_lock(pop, env, &bp->mutex);

	pmemobj_free(bp->test);

	pmemobj_tx_commit();
}

void do_test_inner_transactions(PMEMobjpool *pop) {
	struct base *bp = pmemobj_root_direct(pop, sizeof (*bp));
	jmp_buf env;
	int i;

	if (setjmp(env)) {
		code_not_reached();
		return;
	}
	pmemobj_tx_begin_lock(pop, env, &bp->mutex);

	PMEMoid value = pmemobj_alloc(sizeof (int));
	int *ptr_value = pmemobj_direct(value);
	*ptr_value = 0;

	for (i = 0; i < TEST_INNER_LOOPS; ++i) {
		pmemobj_tx_begin(pop, env);
		int a = *ptr_value;
		a += TEST_VALUE_A;
		pmemobj_memcpy(ptr_value, &a, sizeof (int));
		pmemobj_tx_commit();
	}

	for (i = 0; i < TEST_INNER_LOOPS; ++i) {
		pmemobj_tx_begin(pop, env);
		int b = *ptr_value;
		b -= TEST_VALUE_B;
		pmemobj_memcpy(ptr_value, &b, sizeof (int));
		pmemobj_tx_commit();
	}

	assert(*ptr_value ==
		(TEST_VALUE_A * TEST_INNER_LOOPS -
		TEST_VALUE_B * TEST_INNER_LOOPS));

	pmemobj_free(value);

	pmemobj_tx_commit();
}

void
do_test_abort_alloc_single_transaction(PMEMobjpool *pop)
{
	struct base *bp = pmemobj_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env)) {
		code_not_reached();
		return;
	}

	pmemobj_tx_begin_lock(pop, env, &bp->mutex);

	bp->test = pmemobj_alloc(sizeof (int));
	int *ptr_test = pmemobj_direct(bp->test);
	*ptr_test = TEST_VALUE_A;
	pmemobj_tx_abort(0);
}

void
do_test_abort_set_single_transaction(PMEMobjpool *pop)
{
	struct base *bp = pmemobj_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env)) {
		code_not_reached();
		return;
	}

	pmemobj_tx_begin_lock(pop, env, &bp->mutex);

	bp->test = pmemobj_alloc(sizeof (int));
	int *ptr_test = pmemobj_direct(bp->test);
	*ptr_test = TEST_VALUE_A;
	pmemobj_tx_commit();

	pmemobj_tx_begin_lock(pop, env, &bp->mutex);
	int b = TEST_VALUE_B;
	pmemobj_memcpy(ptr_test, &b, sizeof (int));
	pmemobj_tx_abort(0);

	assert(*ptr_test == TEST_VALUE_A);

	pmemobj_tx_begin_lock(pop, env, &bp->mutex);
	pmemobj_free(bp->test);
	pmemobj_tx_commit();
}

void
do_test_abort_delete_single_transaction(PMEMobjpool *pop)
{
	struct base *bp = pmemobj_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env)) {
		code_not_reached();
		return;
	}

	pmemobj_tx_begin_lock(pop, env, &bp->mutex);

	bp->test = pmemobj_alloc(sizeof (int));
	int *ptr_test = pmemobj_direct(bp->test);
	*ptr_test = TEST_VALUE_A;
	pmemobj_tx_commit();

	pmemobj_tx_begin_lock(pop, env, &bp->mutex);
	pmemobj_free(bp->test);
	pmemobj_tx_abort(0);

	assert(*ptr_test == TEST_VALUE_A);
}

void
do_test_abort_inner_transactions(PMEMobjpool *pop)
{
	struct base *bp = pmemobj_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env)) {
		code_not_reached();
		return;
	}
	pmemobj_tx_begin_lock(pop, env, &bp->mutex);
	bp->test = pmemobj_alloc(sizeof (int));
	int *ptr_test = pmemobj_direct(bp->test);
	*ptr_test = 0;
	pmemobj_tx_commit();

	pmemobj_tx_begin_lock(pop, env, &bp->mutex);
	int a = TEST_VALUE_A;
	pmemobj_memcpy(ptr_test, &a, sizeof (int));

	pmemobj_tx_begin(pop, env);
	{
		int b = TEST_VALUE_B;
		pmemobj_memcpy(ptr_test, &b, sizeof (int));
	}
	pmemobj_tx_abort(0);

	assert(*ptr_test == 0);

	pmemobj_tx_begin_lock(pop, env, &bp->mutex);
	pmemobj_free(bp->test);
	pmemobj_tx_commit();
}

int
main(int argc, char **argv)
{
	START(argc, argv, "obj_basic");

	if (argc < 2)
		FATAL("usage: %s file", argv[0]);

	PMEMobjpool *pop = pmemobj_pool_open(argv[1]);

	do_test_alloc_single_transaction(pop);
	do_test_alloc_huge_single_transaction(pop);
	do_test_set_single_transaction(pop);
	do_test_delete_single_transaction(pop);
	do_test_combine_two_transactions(pop);
	do_test_inner_transactions(pop);
	do_test_abort_alloc_single_transaction(pop);
	do_test_abort_set_single_transaction(pop);
	do_test_abort_delete_single_transaction(pop);
	do_test_abort_inner_transactions(pop);

	/* all done */
	pmemobj_pool_close(pop);

	DONE(NULL);
}
