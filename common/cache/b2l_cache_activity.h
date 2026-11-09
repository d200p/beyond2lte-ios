/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef B2L_CACHE_ACTIVITY_H
#define B2L_CACHE_ACTIVITY_H
#include <stdint.h>
/* Monotonic deadlines only, never input coordinates or user data. */
static inline void b2l_cache_activity_note(uint64_t *deadline,uint64_t now,uint64_t window){
 if(!window)return;
 uint64_t next=now>UINT64_MAX-window?UINT64_MAX:now+window;
 uint64_t old=__atomic_load_n(deadline,__ATOMIC_RELAXED);
 while(old<next&&!__atomic_compare_exchange_n(deadline,&old,next,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED)){}
}
static inline int b2l_cache_activity_active(const uint64_t *deadline,uint64_t now){
 return now<__atomic_load_n(deadline,__ATOMIC_ACQUIRE);
}
#endif
