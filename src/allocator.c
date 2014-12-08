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
* allocator.c -- allocator implementation implementation
*/

#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <pthread.h>
#include <libpmem.h>
#include <stdio.h>
#include "pmem.h"
#include "allocator.h"

#define KB 1024
#define MB 1024*KB

#define LINE_SIZE 4*MB

#define LINE_OFFSET(base, n)      \
(uintptr_t)(base) +               \
sizeof(struct thread_line_info) + \
((n) * LINE_SIZE)

#define ALIGN(v) (((v) & ~7)+8)
#define ALIGN_HUGE(v) (((v) & ~(LINE_SIZE - 1)) + LINE_SIZE)

#define LINE_INFO_VALID 0x95857284
#define HUGE_INFO_VALID 0x85629667

__thread struct thread_line_info {
  uint64_t valid;
  uint64_t offset;
} *thread_line;

struct huge_info {
  uint64_t valid;
  uint64_t lines;
};

pthread_mutex_t line_lock = PTHREAD_MUTEX_INITIALIZER;

bool
allocator_init(struct allocator_hdr *allocator, uint64_t base_offset, int is_pmem) {
  allocator->base_offset = ALIGN(base_offset);
  allocator->lines_used = 0;
  allocator->is_pmem = is_pmem;

  /*
   * These variables are initialized every time right now,
   * but that might change later on.
   * libpmem_persist(is_pmem, allocator, sizeof (*allocator));
   */
  return true;
}

struct thread_line_info *get_thread_line(struct allocator_hdr *allocator, size_t size) {
  if (thread_line != NULL && (thread_line->offset + size) > LINE_SIZE) {
    thread_line = NULL;
  }

  if (thread_line != NULL)
    return thread_line;

  pthread_mutex_lock(&line_lock);
  while(thread_line == NULL) {
    uint64_t line_idx = allocator->lines_used++;
    thread_line = (void *)LINE_OFFSET(allocator, line_idx);
    if (thread_line->valid == HUGE_INFO_VALID) {
      struct huge_info *huge = (struct huge_info *)thread_line;
      allocator->lines_used += huge->lines - 1;
      thread_line = NULL;
    } else if (thread_line->valid != LINE_INFO_VALID) {
      thread_line->offset = LINE_OFFSET(allocator->base_offset, line_idx);
      thread_line->valid = LINE_INFO_VALID - 1;
      libpmem_persist(allocator->is_pmem, thread_line, sizeof (*thread_line));
      thread_line->valid = LINE_INFO_VALID;
      libpmem_persist(allocator->is_pmem, thread_line, sizeof (*thread_line));
    } else if (thread_line->offset + size > LINE_SIZE) {
      thread_line = NULL;
    }
  }
  pthread_mutex_unlock(&line_lock);

  return thread_line;
}

void
thread_alloc(struct allocator_hdr *allocator, uint64_t *ptr, size_t size) {

  struct thread_line_info *line = get_thread_line(allocator, size);
  size = ALIGN(size);
  *ptr = line->offset;
  line->offset += size;
  libpmem_persist(allocator->is_pmem, thread_line, sizeof (*thread_line));
}

void
huge_alloc(struct allocator_hdr *allocator, uint64_t *ptr, size_t size) {
  size = ALIGN_HUGE(size);
  pthread_mutex_lock(&line_lock);
  struct huge_info *huge =
          (void *)LINE_OFFSET(allocator, allocator->lines_used);
  huge->valid = HUGE_INFO_VALID;
  huge->lines = size / LINE_SIZE;

  *ptr = LINE_OFFSET(allocator->base_offset, allocator->lines_used) +
          (sizeof(struct huge_info));
  libpmem_persist(allocator->is_pmem, huge, sizeof (*huge));
  allocator->lines_used += huge->lines;
  pthread_mutex_unlock(&line_lock);
}

void
pmalloc(struct allocator_hdr *allocator, uint64_t *ptr, size_t size) {
  if (size >= LINE_SIZE - (sizeof(struct thread_line_info))) {
    huge_alloc(allocator, ptr, size);
  } else{
    thread_alloc(allocator, ptr, size);
  }
}

void
pfree(struct allocator_hdr *allocator, uint64_t ptr) {
  /* uint64_t line_idx = ALIGN_LINE(ptr); */
  /* XXX implement freelist bins */
}
