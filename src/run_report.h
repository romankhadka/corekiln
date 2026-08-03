#ifndef COREKILN_RUN_REPORT_H
#define COREKILN_RUN_REPORT_H

#include "system_thermal.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum {
  RUN_STOP_DURATION,
  RUN_STOP_INTERRUPT,
  RUN_STOP_TERMINATE,
  RUN_STOP_GPU_FAILURE,
  RUN_STOP_WAIT_ERROR,
} run_stop_reason;

typedef struct {
  bool cpu_selected;
  uint64_t cpu_work_units;
  bool gpu_selected;
  uint64_t gpu_dispatches;
  system_thermal_state thermal;
} run_snapshot;

typedef struct {
  unsigned int status_interval_seconds;
  struct timespec started_at;
  double next_status_seconds;
  system_thermal_state peak_thermal;
  bool serious_reached;
  double serious_at_seconds;
} run_report;

void run_report_initialize(run_report *report,
                           unsigned int status_interval_seconds,
                           struct timespec started_at,
                           run_snapshot initial);
bool run_report_observe(run_report *report, struct timespec now,
                        run_snapshot snapshot);
void run_report_print_status(const run_report *report, struct timespec now,
                             run_snapshot snapshot);
void run_report_print_final(const run_report *report, struct timespec now,
                            run_stop_reason reason, run_snapshot snapshot);

#endif
