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
