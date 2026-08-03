#include "../../src/run_report.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

int main(void) {
  struct timespec started_at = {.tv_sec = 100, .tv_nsec = 0};
  run_snapshot initial = {
      .cpu_selected = true,
      .cpu_work_units = 0,
      .thermal = COREKILN_THERMAL_NOMINAL,
  };
  run_report report;
  run_report_initialize(&report, 5, started_at, initial);

  struct timespec at_four = {.tv_sec = 104, .tv_nsec = 0};
  if (run_report_observe(&report, at_four, initial)) {
    return 1;
  }

  run_snapshot fair = initial;
  fair.cpu_work_units = 10;
  fair.thermal = COREKILN_THERMAL_FAIR;
  struct timespec at_five = {.tv_sec = 105, .tv_nsec = 0};
  if (!run_report_observe(&report, at_five, fair)) {
    return 2;
  }
  run_report_print_status(&report, at_five, fair);

  run_snapshot serious = fair;
  serious.cpu_work_units = 15;
  serious.thermal = COREKILN_THERMAL_SERIOUS;
  struct timespec at_eight = {.tv_sec = 108, .tv_nsec = 0};
  if (run_report_observe(&report, at_eight, serious)) {
    return 3;
  }

  run_snapshot critical = serious;
  critical.cpu_work_units = 20;
  critical.thermal = COREKILN_THERMAL_CRITICAL;
  struct timespec at_ten = {.tv_sec = 110, .tv_nsec = 0};
  run_report_observe(&report, at_ten, critical);
  run_report_print_final(&report, at_ten, RUN_STOP_DURATION, critical);
  return 0;
}
