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
 * objs_list_strdup.c -- linked list unit test for pmemobjs
 *
 * usage: objs_list_strdup file [string...]
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
insert(PMEMobjs *pop, const char *str)
{
	struct base *bp = pmemobjs_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env)) {
		/* transaction aborted */
		return NULL;
	}

	/* begin a transaction, also acquiring the mutex for the list */
	pmemobjs_begin_mutex(pop, env, &bp->mutex);

	/* allocate the new node to be inserted */
	PMEMoid newoid = pmemobjs_alloc(sizeof (struct node));
	struct node *newnode = pmemobjs_direct_ntx(newoid);

	newnode->str = pmemobjs_strdup(str);
	newnode->next = bp->head;
	PMEMOBJS_SET(bp->head, newoid);

	/* commit the transaction (also drops the mutex when complete) */
	pmemobjs_commit();

	return newnode;
}

/*
 * print -- print the entire list
 */
void
print(PMEMobjs *pop)
{
	struct base *bp = pmemobjs_root_direct(pop, sizeof (*bp));

	OUT("list contains:");

	/* protect the loop below by acquiring the list mutex */
	pmemobjs_mutex_lock(&bp->mutex);

	struct node *np = pmemobjs_direct(bp->head);

	while (np != NULL) {
		OUT("    value \"%s\"", (char *)pmemobjs_direct(np->str));
		np = pmemobjs_direct(np->next);
	}

	pmemobjs_mutex_unlock(&bp->mutex);
}

/*
 * freelist -- free the entire list
 */
int
freelist(PMEMobjs *pop)
{
	struct base *bp = pmemobjs_root_direct(pop, sizeof (*bp));
	jmp_buf env;

	if (setjmp(env)) {
		/* transaction aborted, errno set by library */
		return -1;
	}

	/* begin a transaction, also acquiring the mutex for the list */
	pmemobjs_begin_mutex(pop, env, &bp->mutex);

	/*
	 * Since pmemobjs_free() operates on the object ID, use "noid"
	 * to loop through the list of objects and free them, and use "np"
	 * for direct access to the np->next field while looping.
	 */
	PMEMoid noid = bp->head;
	struct node *np = pmemobjs_direct(noid);

	/* loop through the list, freeing each node */
	while (np != NULL) {
		PMEMoid nextnoid = np->next;

		pmemobjs_free(np->str);
		pmemobjs_free(noid);

		noid = nextnoid;
		np = pmemobjs_direct(noid);
	}

	/* commit the transaction, all the frees become permanent now */
	pmemobjs_commit();

	return 0;
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "objs_list_strdup");

	if (argc < 2)
		FATAL("usage: %s file [string...]", argv[0]);

	int fd = OPEN(argv[1], O_RDWR);

	/* map the "object store" memory pool */
	PMEMobjs *pop = pmemobjs_map(fd);

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
	pmemobjs_unmap(pop);

	DONE(NULL);
}
