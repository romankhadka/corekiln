#include "../../src/gpu_kiln.h"

#include <stdint.h>
#include <sys/event.h>
#include <time.h>
#include <unistd.h>

int main(void) {
  char error[256] = {0};
  int queue = kqueue();
  if (queue == -1) {
    return 1;
  }

  struct kevent failure_event;
  EV_SET(&failure_event, 1, EVFILT_USER, EV_ADD | EV_CLEAR, NOTE_FFNOP, 0,
         NULL);
  if (kevent(queue, &failure_event, 1, NULL, 0, NULL) == -1) {
    close(queue);
    return 2;
  }

  gpu_kiln *kiln = gpu_kiln_create(error, sizeof(error));
  if (kiln == NULL ||
      !gpu_kiln_start(kiln, queue, 1, error, sizeof(error))) {
    gpu_kiln_destroy(kiln);
    close(queue);
    return 3;
  }

  struct timespec delay = {.tv_sec = 0, .tv_nsec = 500000000};
  nanosleep(&delay, NULL);
  gpu_kiln_request_stop(kiln);
  if (!gpu_kiln_join(kiln, error, sizeof(error))) {
    gpu_kiln_destroy(kiln);
    close(queue);
    return 4;
  }

  uint64_t completed = gpu_kiln_completed_dispatches(kiln);
  gpu_kiln_destroy(kiln);
  close(queue);
  return completed > 0 ? 0 : 5;
}
