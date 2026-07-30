#include "cpu_kiln.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  struct cpu_kiln *kiln;
  uint64_t seed;
  volatile uint64_t sink;
} worker_context;

struct cpu_kiln {
  size_t worker_count;
  size_t started_count;
  pthread_t *threads;
  worker_context *contexts;
  atomic_bool stop_requested;
  bool started;
  bool joined;
};

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }

  va_list arguments;
  va_start(arguments, format);
  vsnprintf(error, error_size, format, arguments);
  va_end(arguments);
}

static void *burn_cpu(void *argument) {
  worker_context *context = argument;
  uint64_t state = context->seed;

  while (!atomic_load_explicit(&context->kiln->stop_requested,
                               memory_order_relaxed)) {
    for (unsigned int iteration = 0; iteration < 4096; iteration++) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      state *= UINT64_C(0x9e3779b97f4a7c15);
    }
    context->sink = state;
  }

  return NULL;
}

cpu_kiln *cpu_kiln_create(size_t worker_count, char *error,
                          size_t error_size) {
  if (worker_count == 0) {
    set_error(error, error_size, "CPU worker count must be positive");
    return NULL;
  }

  cpu_kiln *kiln = calloc(1, sizeof(*kiln));
  if (kiln == NULL) {
    set_error(error, error_size, "unable to allocate CPU engine");
    return NULL;
  }

  kiln->threads = calloc(worker_count, sizeof(*kiln->threads));
  kiln->contexts = calloc(worker_count, sizeof(*kiln->contexts));
  if (kiln->threads == NULL || kiln->contexts == NULL) {
    set_error(error, error_size, "unable to allocate CPU worker state");
    cpu_kiln_destroy(kiln);
    return NULL;
  }

  kiln->worker_count = worker_count;
  kiln->joined = true;
  atomic_init(&kiln->stop_requested, false);
  return kiln;
}

bool cpu_kiln_start(cpu_kiln *kiln, char *error, size_t error_size) {
  if (kiln == NULL || kiln->started) {
    set_error(error, error_size, "CPU engine cannot be started");
    return false;
  }

  atomic_store_explicit(&kiln->stop_requested, false, memory_order_relaxed);
  kiln->joined = false;

  for (; kiln->started_count < kiln->worker_count; kiln->started_count++) {
    size_t index = kiln->started_count;
    kiln->contexts[index].kiln = kiln;
    kiln->contexts[index].seed =
        UINT64_C(0x9e3779b97f4a7c15) ^ (uint64_t)(index + 1);

    int thread_error = pthread_create(&kiln->threads[index], NULL, burn_cpu,
                                      &kiln->contexts[index]);
    if (thread_error != 0) {
      set_error(error, error_size, "pthread_create: %s",
                strerror(thread_error));
      cpu_kiln_request_stop(kiln);
      char ignored_error[1];
      cpu_kiln_join(kiln, ignored_error, sizeof(ignored_error));
      return false;
    }
  }

  kiln->started = true;
  return true;
}

void cpu_kiln_request_stop(cpu_kiln *kiln) {
  if (kiln != NULL) {
    atomic_store_explicit(&kiln->stop_requested, true, memory_order_relaxed);
  }
}

bool cpu_kiln_join(cpu_kiln *kiln, char *error, size_t error_size) {
  if (kiln == NULL || kiln->joined) {
    return true;
  }

  cpu_kiln_request_stop(kiln);
  bool success = true;
  for (size_t index = 0; index < kiln->started_count; index++) {
    int thread_error = pthread_join(kiln->threads[index], NULL);
    if (thread_error != 0 && success) {
      set_error(error, error_size, "pthread_join: %s", strerror(thread_error));
      success = false;
    }
  }

  kiln->started_count = 0;
  kiln->started = false;
  kiln->joined = true;
  return success;
}

void cpu_kiln_destroy(cpu_kiln *kiln) {
  if (kiln == NULL) {
    return;
  }

  cpu_kiln_request_stop(kiln);
  char ignored_error[1];
  cpu_kiln_join(kiln, ignored_error, sizeof(ignored_error));
  free(kiln->contexts);
  free(kiln->threads);
  free(kiln);
}

size_t cpu_kiln_worker_count(const cpu_kiln *kiln) {
  return kiln == NULL ? 0 : kiln->worker_count;
}
