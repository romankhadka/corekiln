#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <unistd.h>

typedef struct {
  size_t worker_count;
  unsigned int duration_seconds;
  bool worker_count_set;
  bool duration_set;
} options;

typedef struct {
  uint64_t seed;
  volatile uint64_t sink;
} worker_context;

static atomic_bool stop_requested = false;

static void print_usage(void) {
  puts("Usage: corekiln [--workers N] [--duration SECONDS]");
  puts("");
  puts("Keep macOS logical CPUs continuously busy.");
  puts("");
  puts("Options:");
  puts("  --workers N          Number of worker threads");
  puts("  --duration SECONDS   Stop after a positive whole number of seconds");
  puts("  --help               Show this help");
}

static bool parse_positive_integer(const char *text, uintmax_t maximum,
                                   uintmax_t *parsed) {
  char *end = NULL;
  if (text[0] < '0' || text[0] > '9') {
    return false;
  }

  errno = 0;
  uintmax_t candidate = strtoumax(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || candidate == 0 ||
      candidate > maximum) {
    return false;
  }

  *parsed = candidate;
  return true;
}

static bool parse_options(int argc, char *argv[], options *parsed) {
  *parsed = (options){0};

  for (int index = 1; index < argc; index++) {
    const char *argument = argv[index];

    if (strcmp(argument, "--help") == 0) {
      print_usage();
      exit(0);
    }

    if (strcmp(argument, "--workers") == 0) {
      uintmax_t worker_count = 0;
      if (++index >= argc ||
          !parse_positive_integer(argv[index], SIZE_MAX, &worker_count)) {
        fputs("corekiln: --workers requires a positive integer\n", stderr);
        return false;
      }
      parsed->worker_count = (size_t)worker_count;
      parsed->worker_count_set = true;
      continue;
    }

    if (strcmp(argument, "--duration") == 0) {
      uintmax_t duration = 0;
      if (++index >= argc ||
          !parse_positive_integer(argv[index], UINT_MAX, &duration)) {
        fputs("corekiln: --duration requires a positive integer\n", stderr);
        return false;
      }
      parsed->duration_seconds = (unsigned int)duration;
      parsed->duration_set = true;
      continue;
    }

    fprintf(stderr, "corekiln: unknown option: %s\n", argument);
    return false;
  }

  return true;
}

static bool active_cpu_count(size_t *count) {
  int active = 0;
  size_t length = sizeof(active);

  if (sysctlbyname("hw.activecpu", &active, &length, NULL, 0) == 0 &&
      active > 0) {
    *count = (size_t)active;
    return true;
  }

  long online = sysconf(_SC_NPROCESSORS_ONLN);
  if (online > 0) {
    *count = (size_t)online;
    return true;
  }

  return false;
}

static void *burn_cpu(void *argument) {
  worker_context *context = argument;
  uint64_t state = context->seed;

  while (!atomic_load_explicit(&stop_requested, memory_order_relaxed)) {
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

static int build_stop_queue(const options *configuration) {
  if (signal(SIGINT, SIG_IGN) == SIG_ERR ||
      signal(SIGTERM, SIG_IGN) == SIG_ERR) {
    perror("corekiln: signal");
    return -1;
  }

  int queue = kqueue();
  if (queue == -1) {
    perror("corekiln: kqueue");
    return -1;
  }

  struct kevent changes[3];
  int change_count = 0;
  EV_SET(&changes[change_count++], SIGINT, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
  EV_SET(&changes[change_count++], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);

  if (configuration->duration_set) {
    EV_SET(&changes[change_count++], 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
           NOTE_SECONDS, configuration->duration_seconds, NULL);
  }

  if (kevent(queue, changes, change_count, NULL, 0, NULL) == -1) {
    perror("corekiln: kevent registration");
    close(queue);
    return -1;
  }

  return queue;
}

static bool wait_for_stop(int queue) {
  struct kevent event;

  while (kevent(queue, NULL, 0, &event, 1, NULL) == -1) {
    if (errno != EINTR) {
      perror("corekiln: kevent wait");
      return false;
    }
  }

  return true;
}

static void print_start(const options *configuration) {
  if (configuration->duration_set) {
    printf("corekiln: burning %zu %s for %u %s\n",
           configuration->worker_count,
           configuration->worker_count == 1 ? "worker" : "workers",
           configuration->duration_seconds,
           configuration->duration_seconds == 1 ? "second" : "seconds");
  } else {
    printf("corekiln: burning %zu %s until interrupted\n",
           configuration->worker_count,
           configuration->worker_count == 1 ? "worker" : "workers");
  }
  fflush(stdout);
}

static int run_workers(const options *configuration) {
  int queue = build_stop_queue(configuration);
  if (queue == -1) {
    return 1;
  }

  pthread_t *threads =
      calloc(configuration->worker_count, sizeof(*threads));
  worker_context *contexts =
      calloc(configuration->worker_count, sizeof(*contexts));
  if (threads == NULL || contexts == NULL) {
    fputs("corekiln: unable to allocate worker state\n", stderr);
    free(threads);
    free(contexts);
    close(queue);
    return 1;
  }

  atomic_store_explicit(&stop_requested, false, memory_order_relaxed);
  size_t started = 0;
  int exit_status = 0;

  for (; started < configuration->worker_count; started++) {
    contexts[started].seed =
        UINT64_C(0x9e3779b97f4a7c15) ^ (uint64_t)(started + 1);
    int error =
        pthread_create(&threads[started], NULL, burn_cpu, &contexts[started]);
    if (error != 0) {
      fprintf(stderr, "corekiln: pthread_create: %s\n", strerror(error));
      exit_status = 1;
      break;
    }
  }

  if (exit_status == 0) {
    print_start(configuration);
    if (!wait_for_stop(queue)) {
      exit_status = 1;
    }
  }

  atomic_store_explicit(&stop_requested, true, memory_order_relaxed);
  for (size_t index = 0; index < started; index++) {
    int error = pthread_join(threads[index], NULL);
    if (error != 0) {
      fprintf(stderr, "corekiln: pthread_join: %s\n", strerror(error));
      exit_status = 1;
    }
  }

  free(contexts);
  free(threads);
  close(queue);

  if (exit_status == 0) {
    puts("corekiln: stopped");
  }
  return exit_status;
}

int main(int argc, char *argv[]) {
  options configuration;
  if (!parse_options(argc, argv, &configuration)) {
    return 2;
  }

  if (!configuration.worker_count_set &&
      !active_cpu_count(&configuration.worker_count)) {
    fputs("corekiln: unable to discover active logical CPUs\n", stderr);
    return 1;
  }

  return run_workers(&configuration);
}
