#!/usr/bin/env python3
"""Exercise the actual cache observation entry during gestures and resumption."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
source = (repo / 'xnu-overlay/bsd/kern/b2l_native_cache.c').read_text()
start = source.index('void b2l_native_cache_observe(')
body = source[start:source.index('\nstatic int cache_dequeue', start)]
pre = r'''
#include "b2l_cache_core.h"
#include "b2l_cache_activity.h"
#include <assert.h>
#include <stdio.h>
typedef void *vm_map_t;
typedef void *event_t;
#define QSIZE 8u
#define HINTS 16u
struct request {vm_map_t map;uint64_t pc;uint32_t instruction;};
static struct request queue[QSIZE];
static uint64_t hints[HINTS],queued,dropped,interactive_window,interactive_deadline;
static unsigned ready,queue_lock,pending,head,tail;
static unsigned disabled,enabled,preempt,references,wakeups,map_queries,clock_reads;
static int map_token;
static uint64_t now;
static uint64_t mach_absolute_time(void){clock_reads++;return now;}
static void disable_preemption(void){assert(!preempt);preempt=1;disabled++;}
static void enable_preemption(void){assert(preempt);preempt=0;enabled++;}
static vm_map_t current_map(void){map_queries++;return &map_token;}
static void vm_map_reference(vm_map_t map){assert(preempt&&map==&map_token);references++;}
static void thread_wakeup_one(event_t event){assert(!preempt&&event==(event_t)&queue);wakeups++;}
static void reset(void){
 memset(queue,0,sizeof(queue));memset(hints,0,sizeof(hints));
 queued=dropped=0;ready=1;queue_lock=pending=head=tail=0;
 disabled=enabled=preempt=references=wakeups=map_queries=clock_reads=0;
 interactive_window=750;interactive_deadline=850;now=100;
}
'''
post = r'''
int main(void){
 reset();ready=0;b2l_native_cache_observe(0x1000,0xd65f0fff);
 assert(!clock_reads&&!disabled&&!references&&!wakeups);
 reset();b2l_native_cache_observe(0x1000,0x9ac03000); // PACGA is not persisted without the mask ABI.
 assert(!clock_reads&&!disabled&&!references&&!wakeups);
 reset();head=2;pending=2;references=2;
 queue[0]=(struct request){&map_token,0x1000,0xd65f0fff};queue[1]=(struct request){&map_token,0x2000,0xd65f0fff};
 struct request saved[QSIZE];memcpy(saved,queue,sizeof(saved));queue_lock=1;
 for(unsigned i=0;i<1000000;i++)b2l_native_cache_observe(0x3000,0xd65f0fff);
 assert(!disabled&&!enabled&&!map_queries&&!wakeups&&!queued&&!dropped&&references==2);
 assert(head==2&&!tail&&pending==2&&queue_lock==1&&!memcmp(saved,queue,sizeof(saved)));
 queue_lock=0;now=850;b2l_native_cache_observe(0x3000,0xd65f0fff);
 assert(disabled==1&&enabled==1&&references==3&&wakeups==1&&queued==1&&!dropped);
 assert(head==3&&!tail&&pending==3&&queue[2].pc==0x3000);
 assert(!memcmp(saved,queue,sizeof(struct request)*2));
 // Disabled priority retains the original learning and duplicate behavior.
 reset();interactive_window=0;b2l_native_cache_observe(0x1000,0xd65f0fff);
 assert(!clock_reads&&queued==1&&references==1&&wakeups==1);
 b2l_native_cache_observe(0x1000,0xd65f0fff);assert(queued==1&&references==1&&wakeups==1);
 // Outside input, queue contention and full-queue rejection remain bounded.
 reset();now=850;queue_lock=1;b2l_native_cache_observe(0x1000,0xd65f0fff);
 assert(dropped==1&&!references&&!wakeups&&!preempt&&queue_lock==1);
 reset();now=850;head=QSIZE-1;pending=QSIZE-1;b2l_native_cache_observe(0x1000,0xd65f0fff);
 assert(dropped==1&&!references&&!wakeups&&!preempt&&head==QSIZE-1&&pending==QSIZE-1);
 puts("Actual cache observer: one million interactive observations avoid queue work, retain references and resume safely");
}
'''
with tempfile.TemporaryDirectory(prefix='b2l-cache-interactive-') as temporary:
    root = Path(temporary)
    c, exe = root / 'test.c', root / 'test'
    c.write_text(pre + body + post)
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-O1', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                    '-I' + str(repo / 'common/cache'), str(c),
                    str(repo / 'common/cache/b2l_cache_core.c'),
                    str(repo / 'common/cache/CacheSha.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
