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
 * obj_list_basic.c -- linked list unit test for pmemobj
 *
 * usage: obj_list_basic file [val...]
 *
 * The "val" arguments are integers, which are inserted at the beginning
 * of the list.  If the special val "f" is ever encountered, the list
 * is freed and continues with an empty list from that point.
 */

#include "unittest.h"

/* struct node is the element in the linked list */
struct node {
	PMEMoid next;		/* object ID of next struct node */
	int value;		/* payload for this node */
};

/* struct base keeps track of the beginning of the list */
struct base {
	PMEMoid head;		/* object ID of first struct node in list */
	PMEMmutex mutex;	/* lock covering entire list */
};

/*
 * insert -- allocate a new node, and prepend it to the list
 */
struct node *
insert(PMEMobjpool *pop, int val)
{
	struct base *bp = pmemobj_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env)) {
		/*
		 * If we get here, the transaction was aborted due to an
		 * error.  For this simple example, the possible errors would
		 * be an error trying to grab the mutex during the call to
		 * pmemobj_begin_mutex() below, out of memory during the
		 * call to pmemobj_alloc() below, or out of undo log space
		 * during the call to PMEMOBJ_SET() below.
		 *
		 * errno is set by library in this case.
		 *
		 * No additional cleanup to do for this case -- all
		 * allocations and changes that were part of the
		 * transaction are rolled back at this point.
		 */
		return NULL;
	}

	/* begin a transaction, also acquiring the mutex for the list */
	pmemobj_tx_begin_lock(pop, env, &bp->mutex);

	/* allocate the new node to be inserted */
	PMEMoid newoid = pmemobj_alloc(sizeof (struct node));
	struct node *newnode = pmemobj_direct_ntx(newoid);

	/*
	 * Now we have two ways to refer to the new node:
	 *
	 *	newoid is the object ID.  We can't dereference that
	 *	directly but when we point to the new node in pmem,
	 *	we do it by setting bp->head to the object ID, newoid.
	 *
	 *	newnode is the struct node *. Fetching from it works
	 *	as expected, so you could write, for example:
	 *		val = newnode->val;
	 *	to read from newnode.  You just can't store the
	 *	pointer newnode somewhere persistent and expect it
	 *	to work next time the program runs -- only object IDs
	 *	work across program runs.
	 *
	 *	Since pmemobj_direct_ntx() was used, a
	 *	non-transactional pointer to newoid was returned
	 *	which means you can also store to it, but no undo
	 *	log is kept.  So when you do:
	 *		newnode->val = val;
	 *	the value is stored directly in newnode, and if
	 *	the transaction aborts, the newnode allocation is
	 *	undone so there's no need to worry about rolling back
	 *	the store.
	 *
	 *	On the other hand, when bp->head is stored below,
	 *	that's not a new allocation that was part of this
	 *	transaction (bp already existed), so you cannot
	 *	store directly to bp->head, you must use the
	 *	transactional store via the PMEMOBJ_SET() macro.
	 */

	newnode->value = val;
	newnode->next = bp->head;
	PMEMOBJ_SET(bp->head, newoid);

	/* commit the transaction (also drops the mutex when complete) */
	pmemobj_tx_commit();

	return newnode;
}

/*
 * print -- print the entire list
 */
void
print(PMEMobjpool *pop)
{
	struct base *bp = pmemobj_root_direct(pop, sizeof (*bp));

	OUT("list contains:");

	/* protect the loop below by acquiring the list mutex */
	pmemobj_mutex_lock(&bp->mutex);

	struct node *np = pmemobj_direct(bp->head);

	while (np != NULL) {
		OUT("    value %d", np->value);
		np = pmemobj_direct(np->next);
	}

	pmemobj_mutex_unlock(&bp->mutex);
}

/*
 * freelist -- free the entire list
 */
int
freelist(PMEMobjpool *pop)
{
	struct base *bp = pmemobj_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env)) {
		/* transaction aborted, errno set by library */
		return -1;
	}

	/* begin a transaction, also acquiring the mutex for the list */
	pmemobj_tx_begin_lock(pop, env, &bp->mutex);

	/*
	 * Since pmemobj_free() operates on the object ID, use "noid"
	 * to loop through the list of objects and free them, and use "np"
	 * for direct access to the np->next field while looping.
	 */
	PMEMoid noid = bp->head;
	struct node *np = pmemobj_direct(noid);

	/* loop through the list, freeing each node */
	while (np != NULL) {
		PMEMoid nextnoid = np->next;

		pmemobj_free(noid);

		noid = nextnoid;
		np = pmemobj_direct(noid);
	}

	/* commit the transaction, all the frees become permanent now */
	pmemobj_tx_commit();

	return 0;
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "obj_list_basic");

	if (argc < 2)
		FATAL("usage: %s file [val...]", argv[0]);

	PMEMobjpool *pop = pmemobj_pool_open(argv[1]);

	/* if any values were provided, add them to the list */
	for (int i = 2; i < argc; i++)
		if (*argv[i] == 'f') {
			if (freelist(pop) < 0)
				ERR("!freelist");
			else
				OUT("list freed");
		} else {
			int val = atoi(argv[i]);

			if (insert(pop, val) == NULL)
				ERR("!insert on value %d", val);
			else
				OUT("value %d inserted", val);
		}

	/* print the entire list */
	print(pop);

	/* all done */
	pmemobj_pool_close(pop);

	DONE(NULL);
}
