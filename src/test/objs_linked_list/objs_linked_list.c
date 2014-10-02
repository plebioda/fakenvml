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
 * objs_linked_list.c -- linked list unit test for pmemobjs
 *
 * usage: objs_linked_list file
 */

#include "unittest.h"

/* struct node is the element in the linked list */
struct node {
	PMEMoid next;		/* object ID of next struct node */
	int data;
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
insert(PMEMobjs *pop, int d)
{
	struct base *bp = pmemobjs_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env)) {
		/*
		 * If we get here, the transaction was aborted due to an
		 * error.  For this simple example, the possible errors would
		 * be an error trying to grab the mutex during the call to
		 * pmemobjs_begin_mutex() below, out of memory during the
		 * call to pmemobjs_alloc() below, or out of undo log space
		 * during the call to PMEMOBJS_SET() below.
		 *
		 * errno is set by library in this case.
		 *
		 * No additional cleanup to do for this case -- all
		 * allocations and changes that were part of the
		 * transaction are rolled back at this point.
		 */
		return NULL;
	}

	pmemobjs_begin_mutex(pop, env, &bp->mutex);

	PMEMoid newoid = pmemobjs_alloc(sizeof (struct node));
	struct node *newnode = pmemobjs_direct_ntx(newoid);

	/*
	 * Now we have two ways to refer to the new node:
	 *
	 *	newoid is the object ID.  We can't dereference that
	 *	directly but when we point to the new node in pmem,
	 *	we do it by setting bp->head to the object ID, newoid.
	 *
	 *	newnode is the struct node *. Fetching from it works
	 *	as expected, so you could write, for example:
	 *		d = newnode->d;
	 *	to read from newnode.  You just can't store the
	 *	pointer newnode somewhere persistent and expect it
	 *	to work next time the program runs -- only object IDs
	 *	work across program runs.
	 *
	 *	Since pmemobjs_direct_ntx() was used, a
	 *	non-transactional pointer to newoid was returned
	 *	which means you can also store to it, but no undo
	 *	log is kept.  So when you do:
	 *		newnode->d = d;
	 *	the value is stored directly in newnode, and if
	 *	the transaction aborts, the newnode allocation is
	 *	undone so there's no need to worry about rolling back
	 *	the store.
	 *
	 *	On the other hand, when bp->head is stored below,
	 *	that's not a new allocation that was part of this
	 *	transaction (bp already existed), so you cannot
	 *	store directly to bp->head, you must use the
	 *	transactional store via the PMEMOBJS_SET() macro.
	 */

	newnode->data = d;
	newnode->next = bp->head;
	PMEMOBJS_SET(bp->head, newoid);

	pmemobjs_commit();

	return newnode;
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "objs_linked_list");

	if (argc !=  2)
		FATAL("usage: %s file", argv[0]);

	int fd = OPEN(argv[1], O_RDWR);

	PMEMobjs *pop = pmemobjs_map(fd);

	struct node *np = insert(pop, 1);

	ASSERT(np != NULL);

	pmemobjs_unmap(pop);

	DONE(NULL);
}
