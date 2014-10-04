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
 * obj_list_strdup.c -- linked list unit test for pmemobj
 *
 * usage: obj_list_strdup file [string...]
 *
 * The "val" arguments are strings, which are inserted at the beginning
 * of the list.  If the special string "f" is ever encountered, the list
 * is freed and continues with an empty list from that point.
 */

#include "unittest.h"

/* struct node is the element in the linked list */
struct node {
	PMEMoid next;		/* object ID of next struct node */
	PMEMoid str;		/* payload: pmem object containing a string */
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
insert(PMEMobjpool *pop, const char *str)
{
	struct base *bp = pmemobj_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env) == 0) {
		/* try the transaction... */
		pmemobj_tx_begin_lock(pop, env, &bp->mutex);

		/* allocate the new node to be inserted */
		PMEMoid newoid = pmemobj_alloc(sizeof (struct node));
		struct node *newnode = pmemobj_direct_ntx(newoid);

		/* fill it in and link it in */
		newnode->str = pmemobj_strdup(str);
		newnode->next = bp->head;
		PMEMOBJ_SET(bp->head, newoid);

		pmemobj_tx_commit();

		return newnode;
	} else {
		/* transaction aborted */
		return NULL;
	}
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
		OUT("    value \"%s\"", (char *)pmemobj_direct(np->str));
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

	if (setjmp(env) == 0) {
		/* try the transaction... */
		pmemobj_tx_begin_lock(pop, env, &bp->mutex);

		PMEMoid noid = bp->head;
		struct node *np = pmemobj_direct(noid);

		/* loop through the list, freeing each node */
		while (np != NULL) {
			PMEMoid nextnoid = np->next;

			pmemobj_free(np->str);
			pmemobj_free(noid);

			noid = nextnoid;
			np = pmemobj_direct(noid);
		}

		pmemobj_tx_commit();
	} else {
		/* transaction aborted */
		return -1;
	}

	return 0;
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "obj_list_strdup");

	if (argc < 2)
		FATAL("usage: %s file [string...]", argv[0]);

	PMEMobjpool *pop = pmemobj_pool_open(argv[1]);

	/* if any values were provided, add them to the list */
	for (int i = 2; i < argc; i++)
		if (strcmp(argv[i], "f") == 0) {
			if (freelist(pop) < 0)
				ERR("!freelist");
			else
				OUT("list freed");
		} else {
			if (insert(pop, argv[i]) == NULL)
				ERR("!insert on value \"%s\"", argv[i]);
			else
				OUT("value \"%s\" inserted", argv[i]);
		}

	/* print the entire list */
	print(pop);

	/* all done */
	pmemobj_pool_close(pop);

	DONE(NULL);
}
