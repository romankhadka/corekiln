#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t worker_count;
  unsigned int duration_seconds;
  bool worker_count_set;
  bool duration_set;
} options;

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

int main(int argc, char *argv[]) {
  options configuration;
  if (!parse_options(argc, argv, &configuration)) {
    return 2;
  }

  (void)configuration;
  fputs("corekiln: workload unavailable\n", stderr);
  return 3;
}
