#include "config.h"
#include "memory.h"
#include "schedule.h"
#include "sync.h"

struct spinlock *__outer_spinlocks[CONFIG_MAX_CPUS] = { };

struct __mutex_node {
  struct __mutex_node *next;
  thread_id id;
};

err_code create_mutex(struct mutex *mutex) {
  mutex->head = mutex->tail = NULL;
  create_spinlock(&mutex->mlock);
  create_spinlock(&mutex->ilock);
  return create_mem_pool(sizeof(struct __mutex_node), &mutex->pool);
}

void destroy_mutex(struct mutex *mutex) {
  destroy_mem_pool(&mutex->pool);
}

err_code __sleep_in_mutex(struct mutex *mutex) {
  struct __mutex_node *node = NULL;
  bool acquired;

  acquire_spinlock(&mutex->ilock, 0);
  acquired = acquire_spinlock_int(&mutex->mlock, 1);
  if (!acquired) {
    node = alloc_block(&mutex->pool);
    if (node) {
      node->next = NULL;
      node->id = get_thread();
      if (mutex->head)
        mutex->head->next = node;
      mutex->head = node;
      if (!mutex->tail)
        mutex->tail = node;
      pause_this_thread(&mutex->ilock);
    }
  }
  if (!node)
    release_spinlock(&mutex->ilock);

  return (acquired || node) ? ERR_NONE : ERR_OUT_OF_MEMORY;
}

void __awake_in_mutex(struct mutex *mutex) {
  struct __mutex_node *node;
  err_code err;

  do {
    node = mutex->tail;
    mutex->tail = node->next;
    if (mutex->head == node)
      mutex->head = NULL;
    err = resume_thread(node->id);
    free_block(&mutex->pool, node);
  }
  while (mutex->tail && err);

  if (!mutex->tail)
    release_spinlock_int(&mutex->mlock);
}

struct sleep_node {
  struct sleep_node *next;
  thread_id thread;
  uint64_t ticks;
};

static struct sleep_data {
  struct sleep_node *tail;
  struct mem_pool pool;
  struct spinlock lock;
} sleeping[CONFIG_MAX_CPUS];

static void sleep_timer_proc(UNUSED uint64_t ticks) {
  struct sleep_data *slp = &sleeping[get_cpu()];
  if (slp->tail) {
    struct sleep_node *node = slp->tail;
    slp->tail = slp->tail->next;
    if (slp->tail)
      set_timer_ticks(slp->tail->ticks);
    resume_thread(node->thread);
    free_block(&slp->pool, node);
  }
}

err_code sleep(uint64_t period) {
  struct sleep_data *slp = &sleeping[get_cpu()];
  err_code err = ERR_NONE;

  acquire_spinlock(&slp->lock, 0);
  struct sleep_node *node = alloc_block(&slp->pool);
  if (node) {
    node->thread = get_thread();
    node->ticks = get_ticks() + period / CONFIG_SCHEDULER_TICK_INTERVAL;

    if (!slp->tail || slp->tail->ticks > node->ticks) { // is first to wake up
      node->next = slp->tail, slp->tail = node;
      set_timer_ticks(node->ticks);
    }
    else {
      struct sleep_node *prev = slp->tail;
      while (prev->next && prev->next->ticks <= node->ticks)
        prev = prev->next;
      node->next = prev->next, prev->next = node;
    }

    pause_this_thread(&slp->lock);
  }
  else
    err = ERR_OUT_OF_MEMORY;
  if (err)
    release_spinlock(&slp->lock);

  return err;
}

void init_sync(void) {
  set_timer_proc(sleep_timer_proc);
  for (int i = 0; i < get_cpus(); i++)
    if (create_mem_pool(sizeof(struct sleep_node), &sleeping[i].pool))
      PANIC("Failed to create a memory pool");

  LOG_DEBUG("done");
}
