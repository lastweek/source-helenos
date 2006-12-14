/*
 * Copyright (C) 2006 Ondrej Palkovsky
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <test.h>
#include <mm/slab.h>
#include <print.h>
#include <proc/thread.h>
#include <arch.h>
#include <mm/frame.h>
#include <memstr.h>
#include <synch/condvar.h>
#include <synch/mutex.h>

#define ITEM_SIZE 256

/** Fill memory with 2 caches, when allocation fails,
 *  free one of the caches. We should have everything in magazines,
 *  now allocation should clean magazines and allow for full allocation.
 */
static void totalmemtest(void)
{
	slab_cache_t *cache1;
	slab_cache_t *cache2;
	int i;

	void *data1, *data2;
	void *olddata1=NULL, *olddata2=NULL;
	
	cache1 = slab_cache_create("cache1_tst", ITEM_SIZE, 0, NULL, NULL, 0);
	cache2 = slab_cache_create("cache2_tst", ITEM_SIZE, 0, NULL, NULL, 0);

	printf("Allocating...");
	/* Use atomic alloc, so that we find end of memory */
	do {
		data1 = slab_alloc(cache1, FRAME_ATOMIC);
		data2 = slab_alloc(cache2, FRAME_ATOMIC);
		if (!data1 || !data2) {
			if (data1)
				slab_free(cache1,data1);
			if (data2)
				slab_free(cache2,data2);
			break;
		}
		memsetb((uintptr_t)data1, ITEM_SIZE, 0);
		memsetb((uintptr_t)data2, ITEM_SIZE, 0);
		*((void **)data1) = olddata1;
		*((void **)data2) = olddata2;
		olddata1 = data1;
		olddata2 = data2;
	} while(1);
	printf("done.\n");
	/* We do not have memory - now deallocate cache2 */
	printf("Deallocating cache2...");
	while (olddata2) {
		data2 = *((void **)olddata2);
		slab_free(cache2, olddata2);
		olddata2 = data2;
	}
	printf("done.\n");

	printf("Allocating to cache1...\n");
	for (i=0; i<30; i++) {
		data1 = slab_alloc(cache1, FRAME_ATOMIC);
		if (!data1) {
			printf("Incorrect memory size - use another test.");
			return;
		}
		memsetb((uintptr_t)data1, ITEM_SIZE, 0);
		*((void **)data1) = olddata1;
		olddata1 = data1;
	}
	while (1) {
		data1 = slab_alloc(cache1, FRAME_ATOMIC);
		if (!data1) {
			break;
		}
		memsetb((uintptr_t)data1, ITEM_SIZE, 0);
		*((void **)data1) = olddata1;
		olddata1 = data1;
	}
	printf("Deallocating cache1...");
	while (olddata1) {
		data1 = *((void **)olddata1);
		slab_free(cache1, olddata1);
		olddata1 = data1;
	}
	printf("done.\n");
	slab_print_list();
	slab_cache_destroy(cache1);
	slab_cache_destroy(cache2);
}

static slab_cache_t *thr_cache;
static semaphore_t thr_sem;
static condvar_t thread_starter;
static mutex_t starter_mutex;

#define THREADS 8

static void slabtest(void *priv)
{
	void *data = NULL, *new;

	thread_detach(THREAD);

	mutex_lock(&starter_mutex);
	condvar_wait(&thread_starter,&starter_mutex);
	mutex_unlock(&starter_mutex);
		
	printf("Starting thread #%d...\n",THREAD->tid);

	/* Alloc all */
	printf("Thread #%d allocating...\n", THREAD->tid);
	while (1) {
		/* Call with atomic to detect end of memory */
		new = slab_alloc(thr_cache, FRAME_ATOMIC);
		if (!new)
			break;
		*((void **)new) = data;
		data = new;
	}
	printf("Thread #%d releasing...\n", THREAD->tid);
	while (data) {
		new = *((void **)data);
		*((void **)data) = NULL;
		slab_free(thr_cache, data);
		data = new;
	}
	printf("Thread #%d allocating...\n", THREAD->tid);
	while (1) {
		/* Call with atomic to detect end of memory */
		new = slab_alloc(thr_cache, FRAME_ATOMIC);
		if (!new)
			break;
		*((void **)new) = data;
		data = new;
	}
	printf("Thread #%d releasing...\n", THREAD->tid);
	while (data) {
		new = *((void **)data);
		*((void **)data) = NULL;
		slab_free(thr_cache, data);
		data = new;
	}

	printf("Thread #%d finished\n", THREAD->tid);
	slab_print_list();
	semaphore_up(&thr_sem);
}

static void multitest(int size)
{
	/* Start 8 threads that just allocate as much as possible,
	 * then release everything, then again allocate, then release
	 */
	thread_t *t;
	int i;

	printf("Running stress test with size %d\n", size);
	condvar_initialize(&thread_starter);
	mutex_initialize(&starter_mutex);

	thr_cache = slab_cache_create("thread_cache", size, 0, 
				      NULL, NULL, 
				      0);
	semaphore_initialize(&thr_sem,0);
	for (i = 0; i < THREADS; i++) {  
		if (!(t = thread_create(slabtest, NULL, TASK, 0, "slabtest", false)))
			printf("Could not create thread %d\n", i);
		else
			thread_ready(t);
	}
	thread_sleep(1);
	condvar_broadcast(&thread_starter);

	for (i = 0; i < THREADS; i++)
		semaphore_down(&thr_sem);
	
	slab_cache_destroy(thr_cache);
	printf("Stress test complete.\n");
}

char * test_slab2(void)
{
	printf("Running reclaim single-thread test .. pass 1\n");
	totalmemtest();
	printf("Running reclaim single-thread test .. pass 2\n");
	totalmemtest();
	printf("Reclaim test OK.\n");

	multitest(128);
	multitest(2048);
	multitest(8192);
	printf("All done.\n");
	
	return NULL;
}
