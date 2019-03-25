/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/common/basic_defs.h>
#include <tilck/common/string_util.h>

#include <tilck/kernel/process.h>
#include <tilck/kernel/process_int.h>
#include <tilck/kernel/list.h>
#include <tilck/kernel/kmalloc.h>
#include <tilck/kernel/hal.h>
#include <tilck/kernel/tasklet.h>
#include <tilck/kernel/timer.h>

ATOMIC(task_info *) __current;
ATOMIC(u32) disable_preemption_count = 1;

task_info *kernel_process;
process_info *kernel_process_pi;

list runnable_tasks_list;
list sleeping_tasks_list;
list zombie_tasks_list;

static task_info *tree_by_tid_root;

static u64 idle_ticks;
static int runnable_tasks_count;
static int current_max_pid = -1;
static task_info *idle_task;

int get_curr_task_tid(void)
{
   return __current ? __current->tid : 0;
}

static sptr ti_insert_remove_cmp(const void *a, const void *b)
{
   const task_info *t1 = a;
   const task_info *t2 = b;
   return t1->tid - t2->tid;
}

static sptr ti_find_cmp(const void *obj, const void *valptr)
{
   const task_info *task = obj;
   int searched_tid = *(const int *)valptr;
   return task->tid - searched_tid;
}

typedef struct {

   int lowest_available;
   int lowest_after_current_max;

} create_pid_visit_ctx;

static int create_new_pid_visit_cb(void *obj, void *arg)
{
   task_info *ti = obj;
   create_pid_visit_ctx *ctx = arg;

   if (!is_main_thread(ti))
      return 0; /* skip threads */

   /*
    * Algorithm: we start with lowest_available (L) == 0. When we hit
    * tid == L, that means L is not really the lowest, therefore, we guess
    * the right value of L is L + 1. The first time tid skips one, for example
    * jumping from 3 to 5, the value of L set by the iteration with tid == 3,
    * will stuck. That value will be clearly 4.
    */

   if (ctx->lowest_available == ti->tid)
      ctx->lowest_available = ti->tid + 1;

   /*
    * For lowest_after_current_max (A) the logic is similar.
    * We start with A = current_max_pid + 1. The first time A is == tid, will
    * be when tid is current_max_pid + 1. We continue to update A, until the
    * first whole is found. In case tid never reaches current_max_pid + 1,
    * A will be just be current_max_pid + 1, as expected.
    */

   if (ctx->lowest_after_current_max == ti->tid)
      ctx->lowest_after_current_max = ti->tid + 1;

   return 0;
}

int create_new_pid(void)
{
   ASSERT(!is_preemption_enabled());
   create_pid_visit_ctx ctx = { 0, current_max_pid + 1 };
   int r;

   iterate_over_tasks(&create_new_pid_visit_cb, &ctx);

   r = ctx.lowest_after_current_max <= MAX_PID
         ? ctx.lowest_after_current_max
         : (ctx.lowest_available <= MAX_PID ? ctx.lowest_available : -1);

   if (r >= 0)
      current_max_pid = r;

   return r;
}

int iterate_over_tasks(bintree_visit_cb func, void *arg)
{
   ASSERT(!is_preemption_enabled());

   return bintree_in_order_visit(tree_by_tid_root,
                                 func,
                                 arg,
                                 task_info,
                                 tree_by_tid_node);
}

const char *debug_get_state_name(enum task_state state)
{
   switch (state) {

      case TASK_STATE_INVALID:
         return "invalid";

      case TASK_STATE_RUNNABLE:
         return "runnable";

      case TASK_STATE_RUNNING:
         return "running";

      case TASK_STATE_SLEEPING:
         return "sleeping";

      case TASK_STATE_ZOMBIE:
         return "zombie";

      default:
         NOT_REACHED();
   }
}

static void idle(void)
{
   while (true) {

      ASSERT(is_preemption_enabled());

      idle_ticks++;
      halt();

      if (runnable_tasks_count > 0)
         kernel_yield();
   }
}

void create_kernel_process(void)
{
   static char kernel_proc_buf[sizeof(process_info) + sizeof(task_info)];

   task_info *s_kernel_ti = (task_info *)kernel_proc_buf;
   process_info *s_kernel_pi = (process_info *)(s_kernel_ti + 1);

   list_init(&runnable_tasks_list);
   list_init(&sleeping_tasks_list);
   list_init(&zombie_tasks_list);

   VERIFY(create_new_pid() == 0);

   ASSERT(s_kernel_ti->tid == 0);
   ASSERT(s_kernel_ti->pid == 0);
   ASSERT(s_kernel_pi->parent_pid == 0);

   s_kernel_pi->ref_count = 1;
   s_kernel_ti->pi = s_kernel_pi;
   init_task_lists(s_kernel_ti);
   init_process_lists(s_kernel_pi);

   VERIFY(arch_specific_new_task_setup(s_kernel_ti, NULL));

   s_kernel_ti->running_in_kernel = true;
   memcpy(s_kernel_pi->cwd, "/", 2);

   s_kernel_ti->state = TASK_STATE_SLEEPING;

   kernel_process = s_kernel_ti;
   kernel_process_pi = s_kernel_ti->pi;

   add_task(kernel_process);
   set_current_task(kernel_process);
}

process_info *task_get_pi_opaque(task_info *ti)
{
   if (ti != NULL)
      return ti->pi;

   return NULL;
}

void process_set_tty(process_info *pi, void *t)
{
   pi->proc_tty = t;
}

void init_sched(void)
{
   kernel_process->pi->pdir = get_kernel_pdir();
   idle_task = kthread_create(&idle, NULL);

   if (!idle_task)
      panic("Unable to create the idle_task!");
}

void set_current_task_in_kernel(void)
{
   ASSERT(!is_preemption_enabled());
   get_curr_task()->running_in_kernel = true;
}

static void task_add_to_state_list(task_info *ti)
{
   if (is_tasklet_runner(ti))
      return;

   switch (atomic_load_explicit(&ti->state, mo_relaxed)) {

      case TASK_STATE_RUNNABLE:
         list_add_tail(&runnable_tasks_list, &ti->runnable_node);
         runnable_tasks_count++;
         break;

      case TASK_STATE_SLEEPING:
         list_add_tail(&sleeping_tasks_list, &ti->sleeping_node);
         break;

      case TASK_STATE_RUNNING:
         /* no dedicated list: without SMP there's only one 'running' task */
         break;

      case TASK_STATE_ZOMBIE:
         list_add_tail(&zombie_tasks_list, &ti->zombie_node);
         break;

      default:
         NOT_REACHED();
   }
}

static void task_remove_from_state_list(task_info *ti)
{
   if (is_tasklet_runner(ti))
      return;

   switch (atomic_load_explicit(&ti->state, mo_relaxed)) {

      case TASK_STATE_RUNNABLE:
         list_remove(&ti->runnable_node);
         runnable_tasks_count--;
         ASSERT(runnable_tasks_count >= 0);
         break;

      case TASK_STATE_SLEEPING:
         list_remove(&ti->sleeping_node);
         break;

      case TASK_STATE_RUNNING:
         break;

      case TASK_STATE_ZOMBIE:
         list_remove(&ti->zombie_node);
         break;

      default:
         NOT_REACHED();
   }
}

void task_change_state(task_info *ti, enum task_state new_state)
{
   ASSERT(ti->state != new_state);
   ASSERT(ti->state != TASK_STATE_ZOMBIE);
   DEBUG_ONLY(check_in_no_other_irq_than_timer());

   disable_preemption();
   {
      task_remove_from_state_list(ti);
      ti->state = new_state;
      task_add_to_state_list(ti);
   }
   enable_preemption();
}

void add_task(task_info *ti)
{
   disable_preemption();
   {
      task_add_to_state_list(ti);

      bintree_insert(&tree_by_tid_root,
                     ti,
                     ti_insert_remove_cmp,
                     task_info,
                     tree_by_tid_node);
   }
   enable_preemption();
}

void remove_task(task_info *ti)
{
   disable_preemption();
   {
      ASSERT(ti->state == TASK_STATE_ZOMBIE);

      task_remove_from_state_list(ti);

      bintree_remove(&tree_by_tid_root,
                     ti,
                     ti_insert_remove_cmp,
                     task_info,
                     tree_by_tid_node);

      free_task(ti);
   }
   enable_preemption();
}

void account_ticks(void)
{
   task_info *curr = get_curr_task();
   ASSERT(curr != NULL);

   curr->time_slot_ticks++;
   curr->total_ticks++;

   if (curr->running_in_kernel)
      curr->total_kernel_ticks++;
}

bool need_reschedule(void)
{
   task_info *curr = get_curr_task();
   ASSERT(curr != NULL);

   task_info *tasklet_runner = get_hi_prio_ready_tasklet_runner();

   if (tasklet_runner) {

      if (tasklet_runner == curr)
         return false;

      return true;
   }

   if (curr->time_slot_ticks < TIME_SLOT_TICKS &&
       curr->state == TASK_STATE_RUNNING) {
      return false;
   }

   return true;
}

void schedule_outside_interrupt_context(void)
{
   schedule(-1);
}

NORETURN void switch_to_idle_task(void)
{
   switch_to_task(idle_task, X86_PC_TIMER_IRQ);
}

NORETURN void switch_to_idle_task_outside_interrupt_context(void)
{
   switch_to_task(idle_task, -1);
}

void schedule(int curr_irq)
{
   task_info *selected = NULL;
   task_info *pos;

   ASSERT(!is_preemption_enabled());

   selected = get_hi_prio_ready_tasklet_runner();

   if (selected == get_curr_task())
      return;

   // If we preempted the process, it is still runnable.
   if (get_curr_task()->state == TASK_STATE_RUNNING) {
      task_change_state(get_curr_task(), TASK_STATE_RUNNABLE);
   }

   if (selected)
      switch_to_task(selected, curr_irq);

   list_for_each_ro(pos, &runnable_tasks_list, runnable_node) {

      ASSERT(pos->state == TASK_STATE_RUNNABLE);

      if (pos == idle_task || pos == get_curr_task())
         continue;

      if (!selected || pos->total_ticks < selected->total_ticks) {
         selected = pos;
      }
   }

   if (!selected) {

      if (get_curr_task()->state == TASK_STATE_RUNNABLE) {
         selected = get_curr_task();
         task_change_state(selected, TASK_STATE_RUNNING);
         selected->time_slot_ticks = 0;
         return;
      }

      selected = idle_task;
   }

   switch_to_task(selected, curr_irq);
}

task_info *get_task(int tid)
{
   task_info *res = NULL;

   disable_preemption();
   {
      res = bintree_find(tree_by_tid_root,
                         &tid,
                         ti_find_cmp,
                         task_info,
                         tree_by_tid_node);
   }

   enable_preemption();
   return res;
}
