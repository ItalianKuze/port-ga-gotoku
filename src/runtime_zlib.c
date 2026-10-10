/* HLE for libSceZlib. The API is asynchronous: Inflate queues work and
 * WaitForDone returns the completed request ID. */
#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* Before zlib.h: zconf.h includes <unistd.h> after defining FAR, and on Windows that is
   gpu/shim/unistd.h, whose bbport_windows.h #undefs FAR. Taking the shim first leaves
   zconf.h to define FAR itself. */
#include <unistd.h>
#include <zlib.h>

#define ZLIB_NOT_FOUND ((int32_t)0x81120002)
#define ZLIB_INVALID ((int32_t)0x81120016)
#define ZLIB_NO_SPACE ((int32_t)0x8112001c)
#define ZLIB_TIMED_OUT ((int32_t)0x81120027)
#define ZLIB_NOT_INITIALIZED ((int32_t)0x81120032)
#define ZLIB_ALREADY_INITIALIZED ((int32_t)0x81120033)
#define ZLIB_FATAL ((int32_t)0x811200ff)
#define ZLIB_MAX_OUTPUT (64u * 1024u)
#define ZLIB_OUTPUT_ALIGNMENT (2u * 1024u)

typedef struct Result {
    uint64_t request_id;
    uint32_t length;
    int32_t status;
    struct Result *next;
} Result;

typedef struct Done {
    uint64_t request_id;
    struct Done *next;
} Done;

typedef struct Task {
    uint64_t request_id;
    const void *source;
    uint32_t source_length;
    void *destination;
    uint32_t destination_length;
    Result *result;
    Done *done;
    struct Task *next;
} Task;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t task_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t done_ready = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static int initialized, stopping;
static uint64_t next_request_id;
static Task *task_head, *task_tail;
static Done *done_head, *done_tail;
static Result *result_head;

static void *zlib_worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!task_head && !stopping) pthread_cond_wait(&task_ready, &lock);
        if (stopping || !task_head) {
            pthread_mutex_unlock(&lock);
            return NULL;
        }
        Task *task = task_head;
        task_head = task->next;
        if (!task_head) task_tail = NULL;
        pthread_mutex_unlock(&lock);

        uLongf output_length = task->destination_length;
        int status = uncompress((Bytef *)task->destination, &output_length,
                                (const Bytef *)task->source, task->source_length);
        task->result->length = (uint32_t)output_length;
        task->result->status = status == Z_BUF_ERROR ? ZLIB_NO_SPACE : status == Z_OK ? 0 : ZLIB_FATAL;
        task->result->request_id = task->request_id;
        task->done->request_id = task->request_id;

        pthread_mutex_lock(&lock);
        task->result->next = result_head;
        result_head = task->result;
        if (done_tail) done_tail->next = task->done;
        else done_head = task->done;
        done_tail = task->done;
        pthread_cond_broadcast(&done_ready);
        pthread_mutex_unlock(&lock);
        free(task);
    }
}

static void clear_state(void) {
    while (task_head) {
        Task *next = task_head->next;
        free(task_head->result);
        free(task_head->done);
        free(task_head);
        task_head = next;
    }
    task_tail = NULL;
    while (done_head) {
        Done *next = done_head->next;
        free(done_head);
        done_head = next;
    }
    done_tail = NULL;
    while (result_head) {
        Result *next = result_head->next;
        free(result_head);
        result_head = next;
    }
}

static ABI int32_t sce_zlib_initialize(const void *buffer, uint32_t length) {
    (void)buffer;
    (void)length;
    pthread_mutex_lock(&lock);
    if (initialized) {
        pthread_mutex_unlock(&lock);
        return ZLIB_ALREADY_INITIALIZED;
    }
    clear_state();
    next_request_id = 1;
    stopping = 0;
    int error = pthread_create(&worker, NULL, zlib_worker, NULL);
    if (error) {
        pthread_mutex_unlock(&lock);
        return ZLIB_FATAL;
    }
    initialized = 1;
    pthread_mutex_unlock(&lock);
    return 0;
}

static ABI int32_t sce_zlib_inflate(const void *source, uint32_t source_length,
                                    void *destination, uint32_t destination_length,
                                    uint64_t *request_id) {
    if (!initialized) return ZLIB_NOT_INITIALIZED;
    if (!source || !source_length || !destination || !destination_length || !request_id ||
        destination_length > ZLIB_MAX_OUTPUT || destination_length % ZLIB_OUTPUT_ALIGNMENT)
        return ZLIB_INVALID;
    Task *task = calloc(1, sizeof(*task));
    Result *result = calloc(1, sizeof(*result));
    Done *done = calloc(1, sizeof(*done));
    if (!task || !result || !done) {
        free(task); free(result); free(done);
        return ZLIB_FATAL;
    }
    pthread_mutex_lock(&lock);
    if (!initialized || stopping || !next_request_id) {
        pthread_mutex_unlock(&lock);
        free(task); free(result); free(done);
        return ZLIB_NOT_INITIALIZED;
    }
    task->request_id = next_request_id++;
    task->source = source;
    task->source_length = source_length;
    task->destination = destination;
    task->destination_length = destination_length;
    task->result = result;
    task->done = done;
    if (task_tail) task_tail->next = task;
    else task_head = task;
    task_tail = task;
    *request_id = task->request_id;
    pthread_cond_signal(&task_ready);
    pthread_mutex_unlock(&lock);
    return 0;
}

static ABI int32_t sce_zlib_wait_for_done(uint64_t *request_id, const uint32_t *timeout_ms) {
    if (!request_id) return ZLIB_INVALID;
    pthread_mutex_lock(&lock);
    if (!initialized) {
        pthread_mutex_unlock(&lock);
        return ZLIB_NOT_INITIALIZED;
    }
    struct timespec deadline = {0};
    if (timeout_ms) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += *timeout_ms / 1000;
        deadline.tv_nsec += (long)(*timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000L; }
    }
    while (!done_head && initialized) {
        int error = timeout_ms ? pthread_cond_timedwait(&done_ready, &lock, &deadline)
                               : pthread_cond_wait(&done_ready, &lock);
        if (error == ETIMEDOUT && !done_head) {
            pthread_mutex_unlock(&lock);
            return ZLIB_TIMED_OUT;
        }
        if (error && error != ETIMEDOUT) {
            pthread_mutex_unlock(&lock);
            return ZLIB_FATAL;
        }
    }
    if (!done_head) {
        pthread_mutex_unlock(&lock);
        return ZLIB_NOT_INITIALIZED;
    }
    Done *done = done_head;
    done_head = done->next;
    if (!done_head) done_tail = NULL;
    *request_id = done->request_id;
    free(done);
    pthread_mutex_unlock(&lock);
    return 0;
}

static ABI int32_t sce_zlib_get_result(uint64_t request_id, uint32_t *destination_length,
                                       int32_t *status) {
    if (!initialized) return ZLIB_NOT_INITIALIZED;
    if (!destination_length || !status) return ZLIB_INVALID;
    pthread_mutex_lock(&lock);
    Result *result = result_head;
    while (result && result->request_id != request_id) result = result->next;
    if (!result) {
        pthread_mutex_unlock(&lock);
        return ZLIB_NOT_FOUND;
    }
    *destination_length = result->length;
    *status = result->status;
    pthread_mutex_unlock(&lock);
    return 0;
}

static ABI int32_t sce_zlib_finalize(void) {
    pthread_mutex_lock(&lock);
    if (!initialized) {
        pthread_mutex_unlock(&lock);
        return ZLIB_NOT_INITIALIZED;
    }
    stopping = 1;
    pthread_cond_broadcast(&task_ready);
    pthread_mutex_unlock(&lock);
    if (pthread_join(worker, NULL)) return ZLIB_FATAL;
    pthread_mutex_lock(&lock);
    initialized = 0;
    clear_state();
    pthread_mutex_unlock(&lock);
    return 0;
}

uintptr_t runtime_zlib_resolve(const char *name) {
    if (!strcmp(name, "m1YErdIXCp4#o#k")) return (uintptr_t)sce_zlib_initialize;
    if (!strcmp(name, "6na+Sa-B83w#o#k")) return (uintptr_t)sce_zlib_finalize;
    if (!strcmp(name, "TLar1HULv1Q#o#k")) return (uintptr_t)sce_zlib_inflate;
    if (!strcmp(name, "uB8VlDD4e0s#o#k")) return (uintptr_t)sce_zlib_wait_for_done;
    if (!strcmp(name, "2eDcGHC0YaM#o#k")) return (uintptr_t)sce_zlib_get_result;
    return 0;
}
