/* SPDX-License-Identifier: BSD-2-Clause
 * Where a stuck daemon is. Unslide nothing: the log carries the boot's shared
 * cache address through the GPU hook line, and x16 names the trap on its own.
 * Stacks are copied after the task lock is dropped, because copyinmap can fault.
 */
#ifndef B2L_TASK_DUMP_H
#define B2L_TASK_DUMP_H
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/locks.h>
#include <vm/vm_map.h>
#include <arm/thread.h>

#define B2L_DUMP_THREADS 8
#define B2L_DUMP_FRAMES  14
#define B2L_DUMP_USER_VA 0x0000007fffffffffull

extern queue_head_t tasks;
extern lck_mtx_t tasks_threads_lock;
extern kern_return_t copyinmap(vm_map_t map, vm_map_offset_t from, void *to, vm_size_t length);

struct b2l_dump_thread {
    uint64_t id, pc, lr, fp, x16, wait;
    int state;
};

void
b2l_task_dump(const char *name)
{
    struct b2l_dump_thread found[B2L_DUMP_THREADS];
    unsigned count = 0;
    task_t target = TASK_NULL, task;

    lck_mtx_lock(&tasks_threads_lock);
    queue_iterate(&tasks, task, task_t, tasks) {
        void *proc = get_bsdtask_info(task);
        if (proc != NULL && strcmp(proc_best_name(proc), name) == 0) {
            target = task;
            task_reference(target);
            break;
        }
    }
    lck_mtx_unlock(&tasks_threads_lock);
    if (target == TASK_NULL) {
        b2l_early_trace("B2L TASK %s: not running\n", name);
        return;
    }

    thread_t thread;
    task_lock(target);
    queue_iterate(&target->threads, thread, thread_t, task_threads) {
        if (count == B2L_DUMP_THREADS) {
            break;
        }
        const arm_saved_state_t *state = thread->machine.upcb;
        found[count] = (struct b2l_dump_thread){
            .id = thread->thread_id, .state = thread->state,
            .wait = VM_KERNEL_ADDRHIDE(thread->wait_event),
            .pc = state ? get_saved_state_pc(state) & B2L_DUMP_USER_VA : 0,
            .lr = state ? get_saved_state_lr(state) & B2L_DUMP_USER_VA : 0,
            .fp = state ? get_saved_state_fp(state) : 0,
            .x16 = state ? get_saved_state_reg(state, 16) : 0,
        };
        count++;
    }
    task_unlock(target);

    for (unsigned i = 0; i < count; i++) {
        uint64_t fp = found[i].fp, frames[B2L_DUMP_FRAMES];
        unsigned depth = 0;
        while (depth < B2L_DUMP_FRAMES && fp != 0 && !(fp & 7) && fp < B2L_DUMP_USER_VA) {
            uint64_t pair[2];
            if (copyinmap(target->map, fp, pair, sizeof(pair)) != KERN_SUCCESS) {
                break;
            }
            frames[depth++] = pair[1] & B2L_DUMP_USER_VA;
            fp = pair[0];
        }
        b2l_early_trace("B2L TASK %s: tid=%llu state=0x%x wait=0x%llx x16=%lld pc=0x%llx lr=0x%llx\n", name, found[i].id,
            found[i].state, found[i].wait, (long long)found[i].x16, found[i].pc, found[i].lr);
        for (unsigned f = 0; f < depth; f += 4) {
            b2l_early_trace("B2L TASK %s:  #%u 0x%llx 0x%llx 0x%llx 0x%llx\n", name, f, frames[f],
                f + 1 < depth ? frames[f + 1] : 0, f + 2 < depth ? frames[f + 2] : 0, f + 3 < depth ? frames[f + 3] : 0);
        }
    }
    task_deallocate(target);
}
#endif
