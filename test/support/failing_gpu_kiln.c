#include "../../src/gpu_kiln.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

gpu_kiln *gpu_kiln_create(char *error, size_t error_size) {
  if (error != NULL && error_size > 0) {
    snprintf(error, error_size, "injected GPU preparation failure");
  }
  return NULL;
}

bool gpu_kiln_start(gpu_kiln *kiln, int stop_queue,
                    uintptr_t failure_event, char *error,
                    size_t error_size) {
  (void)kiln;
  (void)stop_queue;
  (void)failure_event;
  (void)error;
  (void)error_size;
  return false;
}

void gpu_kiln_request_stop(gpu_kiln *kiln) {
  (void)kiln;
}

bool gpu_kiln_join(gpu_kiln *kiln, char *error, size_t error_size) {
  (void)kiln;
  (void)error;
  (void)error_size;
  return true;
}

void gpu_kiln_destroy(gpu_kiln *kiln) {
  (void)kiln;
}

const char *gpu_kiln_device_name(const gpu_kiln *kiln) {
  (void)kiln;
  return "";
}

uint64_t gpu_kiln_completed_dispatches(const gpu_kiln *kiln) {
  (void)kiln;
  return 0;
}
