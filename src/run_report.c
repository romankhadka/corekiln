#include "run_report.h"

#include <inttypes.h>
#include <stdio.h>

static double elapsed_seconds(const run_report *report, struct timespec now) {
  double seconds = (double)(now.tv_sec - report->started_at.tv_sec);
  seconds +=
      (double)(now.tv_nsec - report->started_at.tv_nsec) / 1000000000.0;
  return seconds < 0.0 ? 0.0 : seconds;
}

static void record_thermal(run_report *report, double elapsed,
                           system_thermal_state thermal) {
  if (thermal > report->peak_thermal) {
    report->peak_thermal = thermal;
  }
  if (!report->serious_reached && thermal >= COREKILN_THERMAL_SERIOUS) {
    report->serious_reached = true;
    report->serious_at_seconds = elapsed;
  }
}

static const char *stop_reason_name(run_stop_reason reason) {
  switch (reason) {
    case RUN_STOP_DURATION:
      return "duration";
    case RUN_STOP_INTERRUPT:
      return "interrupt";
    case RUN_STOP_TERMINATE:
      return "terminate";
    case RUN_STOP_GPU_FAILURE:
      return "gpu_failure";
    case RUN_STOP_WAIT_ERROR:
      return "wait_error";
  }
  return "wait_error";
}

static void print_progress(run_snapshot snapshot) {
  if (snapshot.cpu_selected) {
    printf(" cpu_work_units=%" PRIu64, snapshot.cpu_work_units);
  }
  if (snapshot.gpu_selected) {
    printf(" gpu_dispatches=%" PRIu64, snapshot.gpu_dispatches);
  }
}

void run_report_initialize(run_report *report,
                           unsigned int status_interval_seconds,
                           struct timespec started_at,
                           run_snapshot initial) {
  *report = (run_report){
      .status_interval_seconds = status_interval_seconds,
      .started_at = started_at,
      .next_status_seconds = (double)status_interval_seconds,
      .peak_thermal = COREKILN_THERMAL_UNKNOWN,
  };
  record_thermal(report, 0.0, initial.thermal);
}

bool run_report_observe(run_report *report, struct timespec now,
                        run_snapshot snapshot) {
  double elapsed = elapsed_seconds(report, now);
  record_thermal(report, elapsed, snapshot.thermal);
  if (elapsed < report->next_status_seconds) {
    return false;
  }

  double interval = (double)report->status_interval_seconds;
  uint64_t intervals =
      (uint64_t)((elapsed - report->next_status_seconds) / interval) + 1;
  report->next_status_seconds += (double)intervals * interval;
  return true;
}

void run_report_print_status(const run_report *report, struct timespec now,
                             run_snapshot snapshot) {
  printf("corekiln: status elapsed=%.1fs thermal=%s",
         elapsed_seconds(report, now), system_thermal_name(snapshot.thermal));
  print_progress(snapshot);
  putchar('\n');
  fflush(stdout);
}

void run_report_print_final(const run_report *report, struct timespec now,
                            run_stop_reason reason, run_snapshot snapshot) {
  printf("corekiln: report elapsed=%.1fs stop=%s peak_thermal=%s serious_at=",
         elapsed_seconds(report, now), stop_reason_name(reason),
         system_thermal_name(report->peak_thermal));
  if (report->serious_reached) {
    printf("%.1fs", report->serious_at_seconds);
  } else {
    fputs("not_reached", stdout);
  }
  print_progress(snapshot);
  putchar('\n');
  fflush(stdout);
}
