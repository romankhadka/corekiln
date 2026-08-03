#include "../../src/cpu_kiln.h"

#include <stdint.h>
#include <time.h>

int main(void) {
  char error[256] = {0};
  cpu_kiln *kiln = cpu_kiln_create(1, error, sizeof(error));
  if (kiln == NULL || !cpu_kiln_start(kiln, error, sizeof(error))) {
    cpu_kiln_destroy(kiln);
    return 1;
  }

  struct timespec delay = {.tv_sec = 0, .tv_nsec = 200000000};
  nanosleep(&delay, NULL);
  cpu_kiln_request_stop(kiln);
  if (!cpu_kiln_join(kiln, error, sizeof(error))) {
    cpu_kiln_destroy(kiln);
    return 2;
  }

  uint64_t completed = cpu_kiln_completed_work_units(kiln);
  cpu_kiln_destroy(kiln);
  return completed > 0 ? 0 : 3;
}
