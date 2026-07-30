#include <stdio.h>
#include <string.h>

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

int main(int argc, char *argv[]) {
  if (argc == 2 && strcmp(argv[1], "--help") == 0) {
    print_usage();
    return 0;
  }

  fputs("corekiln: invalid arguments; use --help\n", stderr);
  return 2;
}
