#ifndef COREKILN_CPU_KILN_H
#define COREKILN_CPU_KILN_H

#include <stdbool.h>
#include <stddef.h>

typedef struct cpu_kiln cpu_kiln;

cpu_kiln *cpu_kiln_create(size_t worker_count, char *error,
                          size_t error_size);
bool cpu_kiln_start(cpu_kiln *kiln, char *error, size_t error_size);
void cpu_kiln_request_stop(cpu_kiln *kiln);
bool cpu_kiln_join(cpu_kiln *kiln, char *error, size_t error_size);
void cpu_kiln_destroy(cpu_kiln *kiln);
size_t cpu_kiln_worker_count(const cpu_kiln *kiln);

#endif
