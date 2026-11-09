/* SPDX-License-Identifier: BSD-2-Clause */
#include "b2l_cache_activity.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
static uint64_t deadline;
static void *hints(void *arg){
 uint64_t base=(uint64_t)(uintptr_t)arg;
 for(unsigned i=0;i<10000;i++)b2l_cache_activity_note(&deadline,base+i,750);
 return NULL;
}
int main(void){
 assert(!b2l_cache_activity_active(&deadline,0));
 b2l_cache_activity_note(&deadline,100,750);
 assert(!b2l_cache_activity_active(&deadline,850));
 assert(b2l_cache_activity_active(&deadline,849));
 b2l_cache_activity_note(&deadline,50,750);assert(deadline==850);
 b2l_cache_activity_note(&deadline,1000,0);assert(deadline==850);
 pthread_t t[8];
 for(unsigned i=0;i<8;i++)assert(!pthread_create(&t[i],NULL,hints,(void*)(uintptr_t)(i*100000u)));
 for(unsigned i=0;i<8;i++)assert(!pthread_join(t[i],NULL));
 assert(deadline==700000+9999+750);
 b2l_cache_activity_note(&deadline,UINT64_MAX-100,750);assert(deadline==UINT64_MAX);
 assert(b2l_cache_activity_active(&deadline,UINT64_MAX-1));
 assert(!b2l_cache_activity_active(&deadline,UINT64_MAX));
 puts("Cache input priority: bounded expiry, disabled gate, stale/concurrent hints and overflow passed");
}
