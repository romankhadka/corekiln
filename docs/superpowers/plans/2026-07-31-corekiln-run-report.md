# Corekiln Run Report Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add opt-in periodic status and a final run report containing thermal
pressure, CPU work units, GPU dispatches, elapsed time, and stop reason.

**Architecture:** Keep `src/corekiln.c` as the lifecycle coordinator. Add a
plain-C reporter that owns timing, thermal peaks, deadlines, and formatting; add
a tiny Objective-C bridge for the public Foundation thermal state; expose
read-only progress getters from the existing CPU and GPU engines. A repeating
kqueue timer samples once per second only when `--status` is enabled.

**Tech Stack:** C11, Objective-C with ARC, pthreads, C11 atomics, kqueue,
Foundation `NSProcessInfo`, Metal, Ruby Minitest, Apple Clang

---

## File Structure

- `src/corekiln.c` — parse `--status`, manage sample events, capture engine
  snapshots, choose a stop reason, and coordinate final reporting.
- `src/cpu_kiln.h` / `src/cpu_kiln.c` — publish and expose per-worker completed
  work units.
- `src/gpu_kiln.h` / `src/gpu_kiln.m` — expose successfully completed Metal
  dispatches through the existing atomic completion counter.
- `src/system_thermal.h` / `src/system_thermal.m` — translate public
  `NSProcessInfo.thermalState` values into a stable C enum and display names.
- `src/run_report.h` / `src/run_report.c` — own monotonic timing, status
  deadlines, peak thermal state, first serious observation, output formatting,
  and stop-reason names.
- `test/support/cpu_progress_harness.c` — prove a CPU worker publishes progress.
- `test/support/gpu_progress_harness.c` — prove completed Metal commands are
  exposed.
- `test/support/run_report_harness.c` — deterministically prove reporter timing,
  peak, and formatting behavior.
- `test/support/failing_gpu_kiln.c` — link-time fake used only to prove startup
  failures do not create zero-work reports.
- `test/corekiln_test.rb` — compile harnesses and exercise the complete CLI,
  status, reports, signals, wrapper, and real engines.
- `bin/corekiln` — compile and rebuild for every new source/header.
- `README.md` — document the status option, counters, thermal semantics, and
  output.

### Task 1: Add the status option contract

**Files:**
- Modify: `test/corekiln_test.rb`
- Modify: `src/corekiln.c`

- [ ] **Step 1: Write failing help and validation tests**

Add `--status SECONDS` to `test_help_describes_the_command`:

```ruby
assert_includes stdout, "--status SECONDS"
```

Extend `test_rejects_non_positive_and_non_numeric_values`:

```ruby
[%w[--status 0], "--status requires a positive integer"],
[%w[--status -1], "--status requires a positive integer"],
[%w[--status often], "--status requires a positive integer"],
[%w[--status 4294967296], "--status requires a positive integer"],
```

Extend `test_rejects_missing_values_and_unknown_options`:

```ruby
[%w[--status], "--status requires a positive integer"],
```

- [ ] **Step 2: Run focused tests and verify RED**

Run:

```sh
rtk ruby test/corekiln_test.rb --name '/help|rejects/'
```

Expected: failures because help omits `--status`, valid status is unknown, and
invalid status values produce the unknown-option error.

- [ ] **Step 3: Add status option state and parsing**

Add two fields to `options`:

```c
unsigned int status_interval_seconds;
bool status_set;
```

Add the help line:

```c
puts("  --status SECONDS     Print status every positive whole number of seconds");
```

Add this parser branch immediately after `--duration`:

```c
if (strcmp(argument, "--status") == 0) {
  uintmax_t interval = 0;
  if (++index >= argc ||
      !parse_positive_integer(argv[index], UINT_MAX, &interval)) {
    fputs("corekiln: --status requires a positive integer\n", stderr);
    return false;
  }
  parsed->status_interval_seconds = (unsigned int)interval;
  parsed->status_set = true;
  continue;
}
```

Do not change runtime output yet. This commit defines only the accepted
interface.

- [ ] **Step 4: Run focused and full tests and verify GREEN**

Run:

```sh
rtk ruby test/corekiln_test.rb --name '/help|rejects/'
rtk ruby test/corekiln_test.rb
rtk git diff --check
```

Expected: all existing tests pass; `--status` is accepted but does not yet
produce reporting output.

- [ ] **Step 5: Commit the option contract**

```sh
rtk git add src/corekiln.c test/corekiln_test.rb
rtk git commit -m "Add Corekiln status option"
```

### Task 2: Expose engine progress

**Files:**
- Create: `test/support/cpu_progress_harness.c`
- Create: `test/support/gpu_progress_harness.c`
- Modify: `test/corekiln_test.rb`
- Modify: `src/cpu_kiln.h`
- Modify: `src/cpu_kiln.c`
- Modify: `src/gpu_kiln.h`
- Modify: `src/gpu_kiln.m`

- [ ] **Step 1: Add native progress harnesses**

Create `test/support/cpu_progress_harness.c`:

```c
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
```

Create `test/support/gpu_progress_harness.c`:

```c
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
```

Add two Minitest methods:

```ruby
def test_cpu_engine_exposes_completed_work
  compile_and_run_harness(
    "test/support/cpu_progress_harness.c",
    "src/cpu_kiln.c",
  )
end

def test_gpu_engine_exposes_completed_dispatches
  compile_and_run_harness(
    "test/support/gpu_progress_harness.c",
    "src/gpu_kiln.m",
    frameworks: %w[Foundation Metal],
  )
end
```

Add this helper under `private`:

```ruby
def compile_and_run_harness(
  harness,
  *sources,
  frameworks: [],
  capture_output: false
)
  Dir.mktmpdir("corekiln-harness") do |directory|
    binary = File.join(directory, "harness")
    arguments = [
      "xcrun",
      "clang",
      *COMPILER_FLAGS,
      File.join(ROOT, harness),
      *sources.map { |source| File.join(ROOT, source) },
      *frameworks.flat_map { |framework| ["-framework", framework] },
      "-o",
      binary,
    ]
    _stdout, stderr, status = Open3.capture3(*arguments)
    assert status.success?, "Compilation failed:\n#{stderr}"

    stdout, stderr, status = Open3.capture3(binary)
    assert status.success?, "Harness failed:\n#{stderr}"
    return [stdout, stderr] if capture_output
  end
end
```

- [ ] **Step 2: Run progress tests and verify RED**

Run:

```sh
rtk ruby test/corekiln_test.rb --name '/engine_exposes/'
```

Expected: compile failures because
`cpu_kiln_completed_work_units` and `gpu_kiln_completed_dispatches` are
undefined.

- [ ] **Step 3: Implement CPU work publication**

Include `<stdint.h>` in `src/cpu_kiln.h` and add:

```c
uint64_t cpu_kiln_completed_work_units(const cpu_kiln *kiln);
```

Add a per-worker atomic:

```c
typedef struct {
  struct cpu_kiln *kiln;
  uint64_t seed;
  volatile uint64_t sink;
  atomic_uint_fast64_t completed_work_units;
} worker_context;
```

Initialize each counter in `cpu_kiln_create`:

```c
for (size_t index = 0; index < worker_count; index++) {
  atomic_init(&kiln->contexts[index].completed_work_units, 0);
}
```

Publish every 256 batches and once on exit:

```c
uint64_t completed_work_units = 0;

while (!atomic_load_explicit(&context->kiln->stop_requested,
                             memory_order_relaxed)) {
  for (unsigned int iteration = 0; iteration < 4096; iteration++) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    state *= UINT64_C(0x9e3779b97f4a7c15);
  }
  context->sink = state;
  completed_work_units++;
  if ((completed_work_units & UINT64_C(255)) == 0) {
    atomic_store_explicit(&context->completed_work_units,
                          completed_work_units, memory_order_relaxed);
  }
}

atomic_store_explicit(&context->completed_work_units, completed_work_units,
                      memory_order_relaxed);
```

Reset the worker counter before `pthread_create` and add the getter:

```c
atomic_store_explicit(&kiln->contexts[index].completed_work_units, 0,
                      memory_order_relaxed);
```

```c
uint64_t cpu_kiln_completed_work_units(const cpu_kiln *kiln) {
  if (kiln == NULL) {
    return 0;
  }

  uint64_t completed = 0;
  for (size_t index = 0; index < kiln->worker_count; index++) {
    completed += atomic_load_explicit(
        &kiln->contexts[index].completed_work_units, memory_order_relaxed);
  }
  return completed;
}
```

- [ ] **Step 4: Expose GPU completions**

Add to `src/gpu_kiln.h`:

```c
uint64_t gpu_kiln_completed_dispatches(const gpu_kiln *kiln);
```

Change the state counter to:

```objc
atomic_uint_fast64_t _completed_commands;
```

Add:

```objc
uint64_t gpu_kiln_completed_dispatches(const gpu_kiln *kiln) {
  if (kiln == NULL) {
    return 0;
  }

  CorekilnGPUState *state = state_for((gpu_kiln *)kiln);
  return atomic_load_explicit(&state->_completed_commands,
                              memory_order_relaxed);
}
```

- [ ] **Step 5: Run progress and full tests and verify GREEN**

Run:

```sh
rtk ruby test/corekiln_test.rb --name '/engine_exposes/'
rtk ruby test/corekiln_test.rb
rtk git diff --check
```

Expected: both native harnesses complete positive work, and all existing tests
still pass.

- [ ] **Step 6: Commit engine progress**

```sh
rtk git add src/cpu_kiln.c src/cpu_kiln.h src/gpu_kiln.h src/gpu_kiln.m \
  test/corekiln_test.rb test/support/cpu_progress_harness.c \
  test/support/gpu_progress_harness.c
rtk git commit -m "Expose Corekiln engine progress"
```

### Task 3: Add thermal and report primitives

**Files:**
- Create: `src/system_thermal.h`
- Create: `src/system_thermal.m`
- Create: `src/run_report.h`
- Create: `src/run_report.c`
- Create: `test/support/run_report_harness.c`
- Modify: `test/corekiln_test.rb`

- [ ] **Step 1: Write a deterministic report harness**

Create `test/support/run_report_harness.c`:

```c
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
```

Add:

```ruby
def test_run_report_tracks_deadlines_and_peak_thermal_state
  stdout, = compile_and_run_harness(
    "test/support/run_report_harness.c",
    "src/run_report.c",
    "src/system_thermal.m",
    frameworks: %w[Foundation],
    capture_output: true,
  )

  assert_equal(
    [
      "corekiln: status elapsed=5.0s thermal=fair cpu_work_units=10",
      "corekiln: report elapsed=10.0s stop=duration " \
        "peak_thermal=critical serious_at=8.0s cpu_work_units=20",
    ],
    stdout.lines(chomp: true),
  )
end
```

- [ ] **Step 2: Run the report test and verify RED**

Run:

```sh
rtk ruby test/corekiln_test.rb \
  --name test_run_report_tracks_deadlines_and_peak_thermal_state
```

Expected: compile failure because the report and thermal files do not exist.

- [ ] **Step 3: Add the public thermal bridge**

Create `src/system_thermal.h`:

```c
#ifndef COREKILN_SYSTEM_THERMAL_H
#define COREKILN_SYSTEM_THERMAL_H

typedef enum {
  COREKILN_THERMAL_UNKNOWN = 0,
  COREKILN_THERMAL_NOMINAL,
  COREKILN_THERMAL_FAIR,
  COREKILN_THERMAL_SERIOUS,
  COREKILN_THERMAL_CRITICAL,
} system_thermal_state;

system_thermal_state system_thermal_current(void);
const char *system_thermal_name(system_thermal_state state);

#endif
```

Create `src/system_thermal.m`:

```objc
#import "system_thermal.h"

#import <Foundation/Foundation.h>

system_thermal_state system_thermal_current(void) {
  switch (NSProcessInfo.processInfo.thermalState) {
    case NSProcessInfoThermalStateNominal:
      return COREKILN_THERMAL_NOMINAL;
    case NSProcessInfoThermalStateFair:
      return COREKILN_THERMAL_FAIR;
    case NSProcessInfoThermalStateSerious:
      return COREKILN_THERMAL_SERIOUS;
    case NSProcessInfoThermalStateCritical:
      return COREKILN_THERMAL_CRITICAL;
  }
  return COREKILN_THERMAL_UNKNOWN;
}

const char *system_thermal_name(system_thermal_state state) {
  switch (state) {
    case COREKILN_THERMAL_NOMINAL:
      return "nominal";
    case COREKILN_THERMAL_FAIR:
      return "fair";
    case COREKILN_THERMAL_SERIOUS:
      return "serious";
    case COREKILN_THERMAL_CRITICAL:
      return "critical";
    case COREKILN_THERMAL_UNKNOWN:
      return "unknown";
  }
  return "unknown";
}
```

- [ ] **Step 4: Add the deterministic reporter API**

Create `src/run_report.h`:

```c
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
```

Create `src/run_report.c` with:

```c
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
  if (!report->serious_reached &&
      thermal >= COREKILN_THERMAL_SERIOUS) {
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
```

- [ ] **Step 5: Run report and full tests and verify GREEN**

Run:

```sh
rtk ruby test/corekiln_test.rb \
  --name test_run_report_tracks_deadlines_and_peak_thermal_state
rtk ruby test/corekiln_test.rb
rtk git diff --check
```

Expected: deterministic status and final lines match exactly; existing CLI tests
remain green.

- [ ] **Step 6: Commit report primitives**

```sh
rtk git add src/run_report.c src/run_report.h src/system_thermal.h \
  src/system_thermal.m test/corekiln_test.rb \
  test/support/run_report_harness.c
rtk git commit -m "Add Corekiln run reporter"
```

### Task 4: Integrate periodic and final reporting

**Files:**
- Create: `test/support/failing_gpu_kiln.c`
- Modify: `test/corekiln_test.rb`
- Modify: `src/corekiln.c`
- Modify: `bin/corekiln`

- [ ] **Step 1: Add failing end-to-end reporting tests**

Add a CPU periodic test:

```ruby
def test_status_reports_cpu_progress_and_duration_stop
  with_compiled_corekiln do |binary|
    stdout, stderr, status = Open3.capture3(
      binary,
      "--cpu",
      "--workers",
      "1",
      "--duration",
      "2",
      "--status",
      "1",
    )

    assert status.success?, stderr
    assert_match(
      /corekiln: status elapsed=\d+\.\ds thermal=(?:unknown|nominal|fair|serious|critical) cpu_work_units=[1-9]\d*/,
      stdout,
    )
    assert_match(
      /corekiln: report elapsed=\d+\.\ds stop=duration .*cpu_work_units=[1-9]\d*/,
      stdout,
    )
    assert_operator(
      stdout.index("corekiln: report"),
      :<,
      stdout.index("corekiln: stopped"),
    )
  end
end
```

Add a short GPU run whose status interval does not fire:

```ruby
def test_short_gpu_run_still_prints_final_report
  with_compiled_corekiln do |binary|
    stdout, stderr, status = Open3.capture3(
      binary,
      "--gpu",
      "--duration",
      "1",
      "--status",
      "10",
    )

    assert status.success?, stderr
    refute_includes stdout, "corekiln: status"
    assert_match(
      /corekiln: report .*stop=duration .*gpu_dispatches=[1-9]\d*/,
      stdout,
    )
  end
end
```

Add a combined report test:

```ruby
def test_combined_report_contains_both_progress_counters
  with_compiled_corekiln do |binary|
    stdout, stderr, status = Open3.capture3(
      binary,
      "--both",
      "--workers",
      "1",
      "--duration",
      "1",
      "--status",
      "10",
    )

    assert status.success?, stderr
    assert_match(
      /corekiln: report .*cpu_work_units=[1-9]\d* gpu_dispatches=[1-9]\d*/,
      stdout,
    )
  end
end
```

Change the existing interrupt test to pass `--status 10`. After the process
joins, read the remaining output once:

```ruby
remaining_output = stdout.read
assert_match(/corekiln: report .*stop=interrupt/, remaining_output)
assert_includes remaining_output, "corekiln: stopped"
```

Add the SIGTERM case:

```ruby
def test_termination_reports_terminate_stop_reason
  with_compiled_corekiln do |binary|
    Open3.popen3(
      binary,
      "--cpu",
      "--workers",
      "1",
      "--status",
      "10",
    ) do |stdin, stdout, stderr, wait|
      stdin.close
      begin
        startup = Timeout.timeout(5) { stdout.gets }
        assert_equal(
          "corekiln: burning CPU (1 worker) until interrupted\n",
          startup,
        )

        Process.kill("TERM", wait.pid)
        Timeout.timeout(5) { wait.join }

        assert wait.value.success?, stderr.read
        remaining_output = stdout.read
        assert_match(
          /corekiln: report .*stop=terminate/,
          remaining_output,
        )
        assert_includes remaining_output, "corekiln: stopped"
      ensure
        if wait.alive?
          begin
            Process.kill("KILL", wait.pid)
          rescue Errno::ESRCH
            nil
          end
          wait.join(2)
        end
      end
    end
  end
end
```

Create `test/support/failing_gpu_kiln.c`:

```c
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
```

Allow `with_compiled_corekiln` to accept a source override:

```ruby
def with_compiled_corekiln(sources: SOURCES)
  Dir.mktmpdir("corekiln-test") do |directory|
    binary = File.join(directory, "corekiln")
    _stdout, stderr, status = Open3.capture3(
      "xcrun",
      "clang",
      *COMPILER_FLAGS,
      *sources,
      "-framework",
      "Foundation",
      "-framework",
      "Metal",
      "-o",
      binary,
    )
    assert status.success?, "Compilation failed:\n#{stderr}"

    yield binary
  end
end
```

Add the startup-failure assertion:

```ruby
def test_engine_startup_failure_does_not_print_zero_work_report
  fake_sources = SOURCES.reject { |source| source.end_with?("gpu_kiln.m") }
  fake_sources << File.join(ROOT, "test/support/failing_gpu_kiln.c")

  with_compiled_corekiln(sources: fake_sources) do |binary|
    stdout, stderr, status = Open3.capture3(
      binary,
      "--gpu",
      "--status",
      "1",
    )

    refute status.success?
    assert_includes stderr, "injected GPU preparation failure"
    refute_includes stdout, "corekiln: report"
  end
end
```

Add `src/run_report.c` and `src/system_thermal.m` to `SOURCES`.

- [ ] **Step 2: Run reporting tests and verify RED**

Run:

```sh
rtk ruby test/corekiln_test.rb --name '/status_reports|final_report|combined_report|interrupt|terminate|startup_failure/'
```

Expected: failures because accepted status intervals are ignored and no report
or progress lines are emitted.

- [ ] **Step 3: Add event identifiers and snapshot capture**

In `src/corekiln.c`, include:

```c
#include "run_report.h"
#include "system_thermal.h"

#include <time.h>
```

Replace the single failure constant with:

```c
static const uintptr_t DURATION_EVENT = 1;
static const uintptr_t GPU_FAILURE_EVENT = 2;
static const uintptr_t STATUS_EVENT = 3;
```

Add:

```c
static run_snapshot take_snapshot(const cpu_kiln *cpu,
                                  const gpu_kiln *gpu) {
  return (run_snapshot){
      .cpu_selected = cpu != NULL,
      .cpu_work_units = cpu_kiln_completed_work_units(cpu),
      .gpu_selected = gpu != NULL,
      .gpu_dispatches = gpu_kiln_completed_dispatches(gpu),
      .thermal = system_thermal_current(),
  };
}

static bool monotonic_now(struct timespec *now) {
  if (clock_gettime(CLOCK_MONOTONIC, now) == 0) {
    return true;
  }
  perror("corekiln: monotonic clock");
  return false;
}
```

Use `DURATION_EVENT` in `arm_timer`. Add:

```c
static bool arm_status_timer(int queue) {
  struct kevent timer;
  EV_SET(&timer, STATUS_EVENT, EVFILT_TIMER, EV_ADD, NOTE_SECONDS, 1, NULL);
  if (kevent(queue, &timer, 1, NULL, 0, NULL) == -1) {
    perror("corekiln: status timer registration");
    return false;
  }
  return true;
}
```

- [ ] **Step 4: Replace boolean waiting with explicit stop reasons**

Replace `wait_for_stop` with:

```c
static run_stop_reason wait_for_stop(int queue, run_report *report,
                                     cpu_kiln *cpu, gpu_kiln *gpu) {
  while (true) {
    struct kevent event;
    if (kevent(queue, NULL, 0, &event, 1, NULL) == -1) {
      if (errno == EINTR) {
        continue;
      }
      perror("corekiln: kevent wait");
      return RUN_STOP_WAIT_ERROR;
    }

    if (event.filter == EVFILT_SIGNAL) {
      return event.ident == SIGINT ? RUN_STOP_INTERRUPT : RUN_STOP_TERMINATE;
    }
    if (event.filter == EVFILT_USER &&
        event.ident == GPU_FAILURE_EVENT) {
      return RUN_STOP_GPU_FAILURE;
    }
    if (event.filter == EVFILT_TIMER &&
        event.ident == DURATION_EVENT) {
      return RUN_STOP_DURATION;
    }
    if (event.filter == EVFILT_TIMER && event.ident == STATUS_EVENT &&
        report != NULL) {
      struct timespec now;
      if (!monotonic_now(&now)) {
        return RUN_STOP_WAIT_ERROR;
      }
      run_snapshot snapshot = take_snapshot(cpu, gpu);
      if (run_report_observe(report, now, snapshot)) {
        run_report_print_status(report, now, snapshot);
      }
    }
  }
}
```

- [ ] **Step 5: Integrate reporter eligibility and final output**

In `run_kilns`, add:

```c
run_report report;
run_report *active_report = NULL;
run_stop_reason stop_reason = RUN_STOP_WAIT_ERROR;
```

After every selected engine starts and after the duration timer is armed:

```c
if (configuration->status_set) {
  struct timespec started_at;
  if (!monotonic_now(&started_at)) {
    goto cleanup;
  }
  run_report_initialize(&report, configuration->status_interval_seconds,
                        started_at, take_snapshot(cpu, gpu));
  if (!arm_status_timer(queue)) {
    goto cleanup;
  }
  active_report = &report;
}
```

Replace the wait call with:

```c
print_start(configuration, cpu, gpu);
stop_reason = wait_for_stop(queue, active_report, cpu, gpu);
if (stop_reason != RUN_STOP_GPU_FAILURE &&
    stop_reason != RUN_STOP_WAIT_ERROR) {
  exit_status = 0;
}
```

During cleanup, request both stops before joining either engine. Store CPU and
GPU join success without printing errors immediately:

```c
bool cpu_joined = cpu_kiln_join(cpu, cpu_error, sizeof(cpu_error));
bool gpu_joined = gpu_kiln_join(gpu, gpu_error, sizeof(gpu_error));
if (!cpu_joined || !gpu_joined) {
  exit_status = 1;
}
```

If the GPU join reports failure after another stop reason, set:

```c
if (!gpu_joined) {
  stop_reason = RUN_STOP_GPU_FAILURE;
}
```

Before destroying engines:

```c
if (active_report != NULL) {
  struct timespec finished_at;
  if (monotonic_now(&finished_at)) {
    run_snapshot final_snapshot = take_snapshot(cpu, gpu);
    run_report_observe(active_report, finished_at, final_snapshot);
    run_report_print_final(active_report, finished_at, stop_reason,
                           final_snapshot);
  } else {
    exit_status = 1;
  }
}
```

Then print stored join errors:

```c
if (!cpu_joined) {
  fprintf(stderr, "corekiln: %s\n", cpu_error);
}
if (!gpu_joined) {
  fprintf(stderr, "corekiln: %s\n", gpu_error);
}
```

Destroy engines, close the queue, and preserve the existing rule that
`corekiln: stopped` prints only when `exit_status == 0`.

- [ ] **Step 6: Update direct and wrapper builds**

Add to the Ruby `SOURCES` array:

```ruby
src/run_report.c
src/system_thermal.m
```

Add freshness checks for all four new files in `bin/corekiln`, and compile:

```sh
"$repository_dir/src/run_report.c" \
"$repository_dir/src/system_thermal.m" \
```

before the Foundation and Metal framework arguments.

- [ ] **Step 7: Run focused and full tests and verify GREEN**

Run:

```sh
rtk ruby test/corekiln_test.rb --name '/status_reports|final_report|combined_report|interrupt|terminate|startup_failure/'
rtk ruby test/corekiln_test.rb
rtk sh -n bin/corekiln
rtk git diff --check
```

Expected: periodic CPU status, short GPU final reporting, combined counters,
interrupt and termination reasons, wrapper builds, and every previous behavior
pass.

- [ ] **Step 8: Commit coordinator integration**

```sh
rtk git add bin/corekiln src/corekiln.c test/corekiln_test.rb \
  test/support/failing_gpu_kiln.c
rtk git commit -m "Report Corekiln run progress"
```

### Task 5: Document and verify run reporting

**Files:**
- Modify: `README.md`
- Modify: `docs/superpowers/plans/2026-07-31-corekiln-run-report.md`

- [ ] **Step 1: Update user documentation**

Add `--status SECONDS` to the README help. Document:

```text
corekiln: status elapsed=5.0s thermal=nominal cpu_work_units=824193 gpu_dispatches=71
corekiln: report elapsed=60.2s stop=duration peak_thermal=serious serious_at=23.0s cpu_work_units=9912461 gpu_dispatches=847
```

Explain:

- one CPU work unit is one 4,096-iteration arithmetic batch;
- one GPU dispatch is one successfully completed Metal command buffer;
- counts prove forward progress rather than utilization or benchmark
  performance;
- thermal pressure is qualitative (`nominal`, `fair`, `serious`, `critical`)
  and is not temperature; and
- no private APIs or elevated privileges are used.

- [ ] **Step 2: Run the complete automated suite**

Run:

```sh
rtk ruby test/corekiln_test.rb
```

Expected: zero failures and zero errors.

- [ ] **Step 3: Run a strict direct build**

Run:

```sh
rtk xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -pthread \
  -fobjc-arc -fblocks \
  src/corekiln.c src/cpu_kiln.c src/gpu_kiln.m \
  src/run_report.c src/system_thermal.m \
  -framework Foundation -framework Metal \
  -o /tmp/corekiln-run-report-verification
```

Expected: exit 0 with no warning output.

- [ ] **Step 4: Run all real reporting modes**

Run:

```sh
rtk /tmp/corekiln-run-report-verification \
  --cpu --workers 1 --duration 3 --status 1
rtk /tmp/corekiln-run-report-verification \
  --gpu --duration 3 --status 1
rtk /tmp/corekiln-run-report-verification \
  --both --workers 1 --duration 3 --status 1
```

Expected for every mode:

- periodic status lines appear approximately once per second;
- thermal names are in the documented set;
- selected-engine counters are positive and increase;
- exactly one final report appears before `corekiln: stopped`; and
- the process exits successfully after its requested duration.

- [ ] **Step 5: Verify compatibility without status**

Run:

```sh
rtk /tmp/corekiln-run-report-verification \
  --both --workers 1 --duration 1
```

Expected: only the existing startup and stopped lines; no status or report.

- [ ] **Step 6: Check source and repository hygiene**

Run:

```sh
rtk sh -n bin/corekiln
rtk git diff --check
rtk git status --short
```

Expected: clean syntax and diff. Before the documentation commit, status lists
only README and this plan checklist.

- [ ] **Step 7: Commit documentation and completion tracking**

```sh
rtk git add README.md docs/superpowers/plans/2026-07-31-corekiln-run-report.md
rtk git commit -m "Document Corekiln run reporting"
```

- [ ] **Step 8: Finish the branch**

Use `superpowers:verification-before-completion`, then
`superpowers:finishing-a-development-branch`. Because the user requested
implementation but did not yet request integration, preserve the completed
worktree unless the user explicitly asks to merge or push.
