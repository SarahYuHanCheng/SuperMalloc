#ifndef ATOMICALLY_H
#define ATOMICALLY_H

#include <sched.h>
#include <stdbool.h>
#include <immintrin.h>

#include "bassert.h"
#include "rng.h"

//#define USE_PTHREAD_MUTEXES
#ifdef USE_PTHREAD_MUTEXES
#include <pthread.h>

typedef pthread_mutex_t lock_t;
#define LOCK_INITIALIZER PTHREAD_MUTEX_INITIALIZER
/* static inline void mylock_acquire(lock_t *mylock) { */
/*   int r = pthread_mutex_lock(mylock); */
/*   assert(r==0); */
/* } */
/* static inline void mylock_release(lock_t *mylock) { */
/*   int r = pthread_mutex_unlock(mylock); */
/*   assert(r==0); */
/* } */
/* static inline bool mylock_subscribe(lock_t *mylock) { */
/*   return mylock->_data.__lock == 0; */
/* } */
/* static inline bool mylock_wait(lock_t *mylock) { */
/*   if (mylock_subscribe(mylock)) return false; */
/*   mylock_ */
/* } */

class mylock_raii {
  lock_t *mylock;
public:
  mylock_raii(lock_t *mylock) : mylock(mylock) {
    pthread_mutex_lock(mylock);
  }
  ~mylock_raii() {
    pthread_mutex_unlock(mylock);
  }
};


template<typename ReturnType, typename... Arguments>
static inline ReturnType atomically(lock_t *mylock,
				    const char *name __attribute__((unused)),
			            void (*predo)(Arguments... args) __attribute__((unused)),
				    ReturnType (*fun)(Arguments... args),
				    Arguments... args) {
  mylock_raii m(mylock);
  ReturnType r = fun(args...);
  return r;
}

#else
struct lock_s { volatile unsigned int l __attribute__((aligned(64))); };
typedef struct lock_s lock_t;
#define LOCK_INITIALIZER {0}

static inline bool mylock_wait(lock_t *mylock) {
  if (1) {
    const int pause_count = 30;
    for (int i = 0; i < pause_count; i++) {
      if (mylock->l == 0) return false;
      _mm_pause();
    }
    while (1) {
      sched_yield();
      if (mylock->l == 0) return true;
    }
  } else {
    bool too_long = false;
    while (mylock->l) {
      if (0==(prandnum()&(1024-1))) {
	sched_yield();
	too_long = true;
      } else {
        _mm_pause();
      }
    }
    return too_long;
  }
}

static inline void mylock_acquire(lock_t *mylock) {
  do {
    mylock_wait(mylock);
  } while (__sync_lock_test_and_set(&mylock->l, 1));
}

static inline void mylock_release(lock_t *mylock) {
  __sync_lock_release(&mylock->l);
}

static inline bool mylock_subscribe(lock_t *mylock) {
  return mylock->l == 0;
}

class mylock_raii {
  lock_t *mylock;
public:
  mylock_raii(lock_t *mylock) : mylock(mylock) {
    mylock_acquire(mylock);
  }
  ~mylock_raii() {
    mylock_release(mylock);
  }
};

extern bool use_transactions;
extern bool do_predo;

#define XABORT_LOCK_HELD 9

#ifdef COVERAGE
#define have_rtm 0
#else
#define have_rtm use_transactions
#endif

struct atomic_stats_s {
  uint64_t atomic_count __attribute__((aligned(64)));
  uint64_t locked_count;
};
extern struct atomic_stats_s atomic_stats;

struct failed_counts_s {
  const char *name;
  unsigned int  code;
  uint64_t count;
};
extern lock_t  failed_counts_mutex;
static const int max_failed_counts = 100;
extern int    failed_counts_n;
extern struct failed_counts_s failed_counts [max_failed_counts];

template<typename ReturnType, typename... Arguments>
static inline ReturnType atomically(lock_t *mylock,
				    const char *name,
			            void (*predo)(Arguments... args),
				    ReturnType (*fun)(Arguments... args),
				    Arguments... args) {
  __sync_fetch_and_add(&atomic_stats.atomic_count, 1);
  unsigned int xr = 0xfffffff2;
  if (have_rtm) {
    // Be a little optimistic: try to run the function without the predo if we the lock looks good
    if (mylock->l == 0) {
      xr = _xbegin();
      if (xr == _XBEGIN_STARTED) {
	if (mylock->l) _xabort(XABORT_LOCK_HELD);
	ReturnType r = fun(args...);
	_xend();
	return r;
      }
    }

    int count = 0;
    while (count < 10) {
      mylock_wait(mylock);
      if (do_predo) predo(args...);
      while (mylock_wait(mylock)) {
	// If the lock was held for a long time, then do the predo code again.
	if (do_predo) predo(args...);
      }
      xr = _xbegin();
      if (xr == _XBEGIN_STARTED) {
	ReturnType r = fun(args...);
	if (mylock->l) _xabort(XABORT_LOCK_HELD);
	_xend();
	return r;
      } else if ((xr & _XABORT_EXPLICIT) && (_XABORT_CODE(xr) == XABORT_LOCK_HELD)) {
	count = 0; // reset the counter if we had an explicit lock contention abort.
	continue;
      } else {
	count++;
	//int backoff = (prandnum()%16) << count;
	for (int i = 1; i < (1<<count); i++) {
	  if (0 == (prandnum()&1023)) {
	    sched_yield();
	  } else {
	    __asm__ volatile("pause");
	  }
	}
      }
    }
  }
  // We finally give up and acquire the lock.
  {
    mylock_raii m(&failed_counts_mutex);
    for (int i = 0; i < failed_counts_n; i++) {
      if (failed_counts[i].name == name && failed_counts[i].code == xr) {
	failed_counts[i].count++;
	goto didit;
      }
    }
    bassert(failed_counts_n < max_failed_counts);
    failed_counts[failed_counts_n++] = (struct failed_counts_s){name, xr, 1};
 didit:;
  }

  __sync_fetch_and_add(&atomic_stats.locked_count, 1);
  if (do_predo) predo(args...);
  mylock_raii mr(mylock);
  ReturnType r = fun(args...);
  return r;
}

struct lock {
  unsigned int l __attribute((aligned(64)));
};
#endif


#define atomic_load(addr) __atomic_load_n(addr, __ATOMIC_CONSUME)
#define atomic_store(addr, val) __atomic_store_n(addr, val, __ATOMIC_RELEASE)

#define prefetch_read(addr) __builtin_prefetch(addr, 0, 3)
#define prefetch_write(addr) __builtin_prefetch(addr, 1, 3)
#define load_and_prefetch_write(addr) ({ __typeof__(*addr) ignore __attribute__((unused)) = atomic_load(addr); prefetch_write(addr); })

static inline void fetch_and_max(uint64_t * ptr, uint64_t val) {
  while (1) {
    uint64_t old = atomic_load(ptr);
    if (val <= old) return;
    if (__sync_bool_compare_and_swap(ptr, old, val)) return;
  }
}

#endif // ATOMICALLY_H