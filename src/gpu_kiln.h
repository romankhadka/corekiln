#ifndef COREKILN_GPU_KILN_H
#define COREKILN_GPU_KILN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gpu_kiln gpu_kiln;

gpu_kiln *gpu_kiln_create(char *error, size_t error_size);
bool gpu_kiln_start(gpu_kiln *kiln, int stop_queue,
                    uintptr_t failure_event, char *error, size_t error_size);
void gpu_kiln_request_stop(gpu_kiln *kiln);
bool gpu_kiln_join(gpu_kiln *kiln, char *error, size_t error_size);
void gpu_kiln_destroy(gpu_kiln *kiln);
const char *gpu_kiln_device_name(gpu_kiln *kiln);

#endif
