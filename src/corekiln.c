#include "cpu_kiln.h"
#include "gpu_kiln.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <unistd.h>

typedef enum {
  KILN_MODE_BOTH,
  KILN_MODE_CPU,
  KILN_MODE_GPU,
} kiln_mode;

typedef struct {
  kiln_mode mode;
  bool mode_set;
  size_t worker_count;
  unsigned int duration_seconds;
  unsigned int status_seconds;
  bool worker_count_set;
  bool duration_set;
  bool status_set;
} options;

static const uintptr_t GPU_FAILURE_EVENT = 2;

static void print_usage(void) {
  puts("Usage: corekiln [--cpu | --gpu | --both] [options]");
  puts("");
  puts("Keep macOS compute resources continuously busy.");
  puts("");
  puts("Modes:");
  puts("  --cpu                Load CPU only");
  puts("  --gpu                Load GPU only");
  puts("  --both               Load CPU and GPU");
  puts("                       Default: --both");
  puts("");
  puts("Options:");
  puts("  --workers N          Number of CPU worker threads");
  puts("  --duration SECONDS   Stop after a positive whole number of seconds");
  puts("  --status SECONDS     Print progress every positive whole number of "
       "seconds");
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

static bool set_mode(options *parsed, kiln_mode mode) {
  if (parsed->mode_set) {
    fputs("corekiln: only one mode may be specified\n", stderr);
    return false;
  }

  parsed->mode = mode;
  parsed->mode_set = true;
  return true;
}

static bool parse_options(int argc, char *argv[], options *parsed) {
  *parsed = (options){.mode = KILN_MODE_BOTH};

  for (int index = 1; index < argc; index++) {
    const char *argument = argv[index];

    if (strcmp(argument, "--help") == 0) {
      print_usage();
      exit(0);
    }
    if (strcmp(argument, "--cpu") == 0) {
      if (!set_mode(parsed, KILN_MODE_CPU)) {
        return false;
      }
      continue;
    }
    if (strcmp(argument, "--gpu") == 0) {
      if (!set_mode(parsed, KILN_MODE_GPU)) {
        return false;
      }
      continue;
    }
    if (strcmp(argument, "--both") == 0) {
      if (!set_mode(parsed, KILN_MODE_BOTH)) {
        return false;
      }
      continue;
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
    if (strcmp(argument, "--status") == 0) {
      uintmax_t status = 0;
      if (++index >= argc ||
          !parse_positive_integer(argv[index], UINT_MAX, &status)) {
        fputs("corekiln: --status requires a positive integer\n", stderr);
        return false;
      }
      parsed->status_seconds = (unsigned int)status;
      parsed->status_set = true;
      continue;
    }

    fprintf(stderr, "corekiln: unknown option: %s\n", argument);
    return false;
  }

  if (parsed->mode == KILN_MODE_GPU && parsed->worker_count_set) {
    fputs("corekiln: --workers requires a CPU mode\n", stderr);
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

static int build_stop_queue(void) {
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
  EV_SET(&changes[0], SIGINT, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
  EV_SET(&changes[1], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
  EV_SET(&changes[2], GPU_FAILURE_EVENT, EVFILT_USER, EV_ADD | EV_CLEAR,
         NOTE_FFNOP, 0, NULL);
  if (kevent(queue, changes, 3, NULL, 0, NULL) == -1) {
    perror("corekiln: kevent registration");
    close(queue);
    return -1;
  }

  return queue;
}

static bool arm_timer(int queue, unsigned int duration_seconds) {
  struct kevent timer;
  EV_SET(&timer, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_SECONDS,
         duration_seconds, NULL);
  if (kevent(queue, &timer, 1, NULL, 0, NULL) == -1) {
    perror("corekiln: timer registration");
    return false;
  }
  return true;
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

static bool mode_uses_cpu(kiln_mode mode) {
  return mode != KILN_MODE_GPU;
}

static bool mode_uses_gpu(kiln_mode mode) {
  return mode != KILN_MODE_CPU;
}

static void print_start(const options *configuration, cpu_kiln *cpu,
                        gpu_kiln *gpu) {
  fputs("corekiln: burning ", stdout);
  if (cpu != NULL) {
    size_t worker_count = cpu_kiln_worker_count(cpu);
    const char *worker_word = worker_count == 1 ? "worker" : "workers";
    printf("CPU (%zu %s)", worker_count, worker_word);
  }
  if (cpu != NULL && gpu != NULL) {
    fputs(" + ", stdout);
  }
  if (gpu != NULL) {
    printf("GPU (%s)", gpu_kiln_device_name(gpu));
  }

  if (configuration->duration_set) {
    const char *second_word =
        configuration->duration_seconds == 1 ? "second" : "seconds";
    printf(" for %u %s\n", configuration->duration_seconds, second_word);
  } else {
    fputs(" until interrupted\n", stdout);
  }
  fflush(stdout);
}

static int run_kilns(const options *configuration) {
  char cpu_error[512] = {0};
  char gpu_error[512] = {0};
  cpu_kiln *cpu = NULL;
  gpu_kiln *gpu = NULL;
  int queue = -1;
  int exit_status = 1;

  if (mode_uses_cpu(configuration->mode)) {
    cpu = cpu_kiln_create(configuration->worker_count, cpu_error,
                          sizeof(cpu_error));
    if (cpu == NULL) {
      fprintf(stderr, "corekiln: %s\n", cpu_error);
      goto cleanup;
    }
  }

  if (mode_uses_gpu(configuration->mode)) {
    gpu = gpu_kiln_create(gpu_error, sizeof(gpu_error));
    if (gpu == NULL) {
      fprintf(stderr, "corekiln: %s\n", gpu_error);
      goto cleanup;
    }
  }

  queue = build_stop_queue();
  if (queue == -1) {
    goto cleanup;
  }

  if (cpu != NULL &&
      !cpu_kiln_start(cpu, cpu_error, sizeof(cpu_error))) {
    fprintf(stderr, "corekiln: %s\n", cpu_error);
    goto cleanup;
  }
  if (gpu != NULL &&
      !gpu_kiln_start(gpu, queue, GPU_FAILURE_EVENT, gpu_error,
                      sizeof(gpu_error))) {
    fprintf(stderr, "corekiln: %s\n", gpu_error);
    goto cleanup;
  }
  if (configuration->duration_set &&
      !arm_timer(queue, configuration->duration_seconds)) {
    goto cleanup;
  }

  print_start(configuration, cpu, gpu);
  if (wait_for_stop(queue)) {
    exit_status = 0;
  }

cleanup:
  cpu_kiln_request_stop(cpu);
  gpu_kiln_request_stop(gpu);

  if (!cpu_kiln_join(cpu, cpu_error, sizeof(cpu_error))) {
    fprintf(stderr, "corekiln: %s\n", cpu_error);
    exit_status = 1;
  }
  if (!gpu_kiln_join(gpu, gpu_error, sizeof(gpu_error))) {
    fprintf(stderr, "corekiln: %s\n", gpu_error);
    exit_status = 1;
  }

  cpu_kiln_destroy(cpu);
  gpu_kiln_destroy(gpu);
  if (queue != -1) {
    close(queue);
  }

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

  if (mode_uses_cpu(configuration.mode)) {
    if (!configuration.worker_count_set &&
        !active_cpu_count(&configuration.worker_count)) {
      fputs("corekiln: unable to discover active logical CPUs\n", stderr);
      return 1;
    }
  }

  return run_kilns(&configuration);
}
