#ifndef RS_CONCERRNT_CTX_
#define RS_CONCERRNT_CTX_

#include "redisearch.h"
#include "redismodule.h"
#include <time.h>
#include <dep/thpool/thpool.h>

/** Concurrent Search Exection Context.
 *
 * We allow queries to run concurrently, each running on its own thread, locking the redis GIL
 * for a bit, releasing it, and letting others run as well.
 *
 * The queries do not really run in parallel, but one at a time, competing over the global lock.
 * This does not speed processing - in fact it can actually slow it down. But it prevents a
 * common situation, where very slow queries block the entire redis instance for a long time.
 *
 * We intend to switch this model to a single thread running multiple "coroutines", but for now
 * this naive implementation is good enough and will fix the search concurrency issue.
 *
 * The ConcurrentSearchCtx is part of a query, and the query calls the CONCURRENT_CTX_TICK macro
 * for every "cycle" - meaning a processed search result. The concurrency engine will switch
 * execution to another query when the current thread has spent enough time working.
 *
 * The current switch threshold is 200 microseconds. Since measuring time is slow in itself (~50ns)
 * we sample the elapsed time every 20 "cycles" of the query processor.
 *
 */

typedef void (*ConcurrentReopenCallback)(RedisModuleKey *k, void *ctx);
typedef struct {
  RedisModuleKey *key;
  RedisModuleString *keyName;
  void *ctx;
  ConcurrentReopenCallback cb;
  int keyFlags;
} ConcurrentKeyCtx;

typedef struct {
  long long ticker;
  struct timespec lastTime;
  RedisModuleCtx *ctx;
  ConcurrentKeyCtx *openKeys;
  size_t numOpenKeys;
} ConcurrentSearchCtx;

/** The maximal size of the concurrent query thread pool. Since only one thread is operational at a
 * time, it's not a problem besides memory consumption, to have much more threads than CPU cores.
 * By default the pool starts with just one thread, and scales up as needed  */
#define CONCURRENT_SEARCH_POOL_SIZE 100

/** The number of execution "ticks" per elapsed time check. This is intended to reduce the number of
 * calls to clock_gettime() */
#define CONCURRENT_TICK_CHECK 25

/** The timeout after which we try to switch to another query thread - in Nanoseconds */
#define CONCURRENT_TIMEOUT_NS 50000

void ConcurrentSearch_AddKey(ConcurrentSearchCtx *ctx, RedisModuleKey *key, int openFlags,
                             RedisModuleString *keyName, ConcurrentReopenCallback cb,
                             void *privdata);
/** Start the concurrent search thread pool. Should be called when initializing the module */
void ConcurrentSearch_ThreadPoolStart();

/* Run a function on the concurrent thread pool */
void ConcurrentSearch_ThreadPoolRun(void (*func)(void *), void *arg);

/** Check the elapsed timer, and release the lock if enough time has passed.
 * Return 1 if switching took place
 */
int ConcurrentSearch_CheckTimer(ConcurrentSearchCtx *ctx);

/** Initialize and reset a concurrent search ctx */
void ConcurrentSearchCtx_Init(RedisModuleCtx *rctx, ConcurrentSearchCtx *ctx);

void ConcurrentSearchCtx_Free(ConcurrentSearchCtx *ctx);

void ConcurrentSearchCtx_Lock(ConcurrentSearchCtx *ctx);

void ConcurrentSearchCtx_Unlock(ConcurrentSearchCtx *ctx);

/** This macro is called by concurrent executors (currently the query only).
 * It checks if enough time has passed and releases the global lock if that is the case.
 */
#define CONCURRENT_CTX_TICK(x)                               \
  ({                                                         \
    int conctx__didSwitch = 0;                               \
    if ((x) && ++(x)->ticker % CONCURRENT_TICK_CHECK == 0) { \
      if (ConcurrentSearch_CheckTimer((x))) {                \
        conctx__didSwitch = 1;                               \
      }                                                      \
    }                                                        \
    conctx__didSwitch;                                       \
  })

#endif