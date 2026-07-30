# Corekiln Run Report Design

## Context

Corekiln can continuously load the CPU, GPU, or both, but it currently reports
only startup and shutdown. A user must open Activity Monitor to observe the run,
and the process does not record when macOS raises thermal pressure or how much
work each engine completed.

The next addition will make a Corekiln run self-observing without changing its
load behavior. Reporting remains opt-in so existing invocations and output stay
stable.

## Goals

- Add periodic, human-readable status output.
- Record macOS thermal pressure throughout the run.
- Report low-overhead CPU and GPU progress counters.
- Identify why the run stopped.
- Always emit a final report when reporting was successfully started.
- Use public macOS APIs only.
- Preserve maximum-load behavior and current output when reporting is disabled.

## Non-goals

- Do not claim an exact CPU or GPU utilization percentage.
- Do not report temperatures, clock speeds, fan speeds, or power draw.
- Do not use private Apple APIs, `powermetrics`, elevated privileges, or a
  helper process.
- Do not reduce load in response to thermal state.
- Do not add JSON, CSV, files, or historical result storage in this change.
- Do not add memory stress or adaptive GPU calibration.

## Command-line Interface

Add one option:

```text
--status SECONDS      Print status every positive whole number of seconds
```

The complete usage becomes:

```text
corekiln [--cpu | --gpu | --both] [--workers N]
         [--duration SECONDS] [--status SECONDS]
```

`--status` accepts the same positive-integer syntax and `UINT_MAX` ceiling as
`--duration`. Missing, zero, negative, nonnumeric, and overflowing values are
rejected before any engine is prepared.

When `--status` is absent, Corekiln preserves its current startup and shutdown
output. When present, it enables both periodic status lines and one final run
report.

Examples:

```sh
bin/corekiln --status 5
bin/corekiln --gpu --duration 60 --status 10
bin/corekiln --cpu --workers 4 --status 1
```

## Output Contract

The existing startup line remains first:

```text
corekiln: burning CPU (14 workers) + GPU (Apple M4 Pro) until interrupted
```

At each requested interval, Corekiln prints a single status line:

```text
corekiln: status elapsed=5.0s thermal=nominal cpu_work_units=824193 gpu_dispatches=71
```

Mode-specific fields are omitted when their engine is not selected:

```text
corekiln: status elapsed=5.0s thermal=fair gpu_dispatches=68
```

After the selected engines have stopped and all in-flight work has drained,
Corekiln prints one final report:

```text
corekiln: report elapsed=60.2s stop=duration peak_thermal=serious serious_at=23.0s cpu_work_units=9912461 gpu_dispatches=847
corekiln: stopped
```

`serious_at` is the elapsed time at which `serious` or `critical` thermal
pressure was first observed. It is `not_reached` if neither state was observed.
If the initial sample is already serious or critical, the value is `0.0s`.

The valid stop values are:

- `duration`
- `interrupt`
- `terminate`
- `gpu_failure`
- `wait_error`

The report is emitted for every stop after both of these conditions hold:

1. all selected engines started successfully; and
2. status timing and the initial thermal sample were initialized.

An engine preparation or startup failure therefore produces the existing error
without a misleading zero-work report. A GPU runtime failure produces a report
with `stop=gpu_failure`, followed by the existing error on standard error; it
does not print `corekiln: stopped`.

All normal startup, status, report, and stopped lines go to standard output.
Errors remain on standard error.

## Thermal Semantics

Corekiln will use Foundation's public `NSProcessInfo.thermalState` property. The
states map directly to:

- `nominal`
- `fair`
- `serious`
- `critical`
- `unknown` for any unrecognized future value

Apple documents thermal state as system-wide thermal pressure and notes that
the system reduces processor speed as thermal pressure increases. It is not a
temperature reading and is not proof of a specific throttling percentage:

- [NSProcessInfo](https://developer.apple.com/documentation/foundation/processinfo?language=objc)
- [NSProcessInfoThermalState](https://developer.apple.com/documentation/foundation/processinfo/thermalstate-swift.enum?changes=_2&language=objc)

When status is enabled, Corekiln samples thermal state once per second. This
gives `serious_at` one-second resolution independent of the user-visible status
interval. The final report takes one last sample after engine joins.

Peak severity uses this ordering:

```text
unknown < nominal < fair < serious < critical
```

`unknown` never replaces a known peak.

## Work Counters

### CPU

One CPU work unit is one completed outer worker-loop batch. A batch contains
4,096 iterations of the existing arithmetic workload.

Each worker owns its progress counter. To avoid a shared atomic hot spot, the
worker publishes progress to a relaxed atomic counter every 256 work units and
once more before exiting. The reporter sums the per-worker counters. Reporting
does not add locks or cross-worker writes to the workload path.

The final CPU count is exact to completed batches. A periodic status count may
lag each worker-local total by at most 255 work units.

Add this engine query:

```c
uint64_t cpu_kiln_completed_work_units(const cpu_kiln *kiln);
```

### GPU

One GPU dispatch is one successfully completed `corekiln_burn` command buffer.
The GPU engine already tracks successful command completion atomically; expose
the count through:

```c
uint64_t gpu_kiln_completed_dispatches(const gpu_kiln *kiln);
```

Failed and cancelled command buffers never increment the count. The final
report is created after the three in-flight slots drain, so it includes every
successfully completed dispatch.

These counters prove forward progress. They are deliberately not presented as
benchmarks or utilization percentages.

## Components

### `src/system_thermal.h` and `src/system_thermal.m`

Expose a small C interface over `NSProcessInfo.thermalState`. The Objective-C
implementation contains all Foundation-specific code and returns a Corekiln
enum plus a stable display name. It owns no thread, timer, or notification
observer.

### `src/run_report.h` and `src/run_report.c`

Own reporting state:

- monotonic start time;
- selected engines;
- current and peak thermal state;
- first serious-or-critical observation time;
- next visible status deadline; and
- whether final reporting is eligible.

The reporter accepts snapshots from the coordinator rather than depending
directly on CPU or GPU engine internals. A snapshot contains the current thermal
state and selected-engine counters. This keeps formatting and peak tracking
independently testable.

### `src/cpu_kiln.c` and `src/gpu_kiln.m`

Expose read-only progress getters. Counter collection must not alter engine
start, stop, join, or failure semantics.

### `src/corekiln.c`

Continue to own orchestration. It will:

1. parse `--status`;
2. prepare the selected engines;
3. build the stop queue;
4. start every selected engine;
5. arm the duration timer, if requested;
6. initialize the reporter and arm a repeating one-second sample timer, if
   requested;
7. print the existing startup line;
8. process stop and sample events;
9. request both engine stops;
10. join both engines;
11. take a final snapshot and print the report;
12. destroy all state and print `corekiln: stopped` on success.

## Event and Timing Model

Use distinct kqueue identifiers:

- duration timer: `1`
- GPU failure user event: `2`
- status sample timer: `3`

The duration timer remains one-shot and is armed only after all selected
engines start. The status timer repeats every second and is armed immediately
after the reporter's monotonic start timestamp is captured.

The wait function will return an explicit stop reason instead of a boolean.
Status timer events produce snapshots and continue waiting. Duration, signal,
GPU failure, and unrecoverable kqueue errors terminate the wait.

Visible status is deadline-based rather than tick-count-based. If event
delivery is delayed, Corekiln emits one current status line and advances the
next deadline; it does not print a burst of stale lines.

The final elapsed time uses `CLOCK_MONOTONIC`, so wall-clock changes do not
distort a run.

## Error Handling

- Invalid `--status` input exits with argument status `2`.
- Failure to initialize monotonic timing or register the sample timer stops and
  joins any started engines, prints the operating-system error, and exits
  nonzero without a report.
- An unknown thermal enum is reported as `unknown` and does not stop the load.
- GPU runtime failure wakes the coordinator through the existing user event,
  drains both engines, emits a final report, prints the GPU error, and exits
  nonzero.
- A signal and duration event arriving together may choose either event as the
  stop reason according to kqueue delivery order; engine cleanup remains
  identical.

## Build and Wrapper

The direct and wrapper builds add:

- `src/system_thermal.h`
- `src/system_thermal.m`
- `src/run_report.h`
- `src/run_report.c`

Foundation is already linked for the Metal engine, so no new framework is
required. The wrapper rebuild freshness list includes all four files.

## Testing

Extend the black-box Minitest suite to cover:

- `--help` documentation for `--status`;
- missing, zero, negative, nonnumeric, and overflowing status intervals;
- unchanged output shape when status is absent;
- periodic CPU-only status with a positive work count;
- GPU-only final reporting with a positive dispatch count;
- combined mode containing both counter fields;
- a run shorter than its status interval still producing a final report;
- duration, interrupt, and termination stop reasons;
- thermal values restricted to the documented display names;
- `serious_at=not_reached` or a valid elapsed duration;
- report-before-stopped ordering on success;
- no final report when engine startup fails; and
- strict direct and wrapper compilation with every new source.

Timed tests use one CPU worker. Tests that require periodic output use a
two-second duration and one-second status interval. GPU and combined tests keep
durations as short as the assertion permits.

Final verification runs:

```sh
ruby test/corekiln_test.rb
bin/corekiln --cpu --workers 1 --duration 3 --status 1
bin/corekiln --gpu --duration 3 --status 1
bin/corekiln --both --workers 1 --duration 3 --status 1
```

## Documentation

Update the README with:

- the new option and examples;
- definitions for CPU work units and GPU dispatches;
- the meaning and limitations of thermal pressure;
- an explicit statement that counts prove progress, not performance or
  utilization; and
- sample periodic and final output.

## Acceptance Criteria

- Existing commands without `--status` preserve current behavior and output.
- `--status N` prints no faster than the requested interval.
- Every eligible run prints exactly one final report.
- CPU and GPU counters are positive after a successful timed run of their
  engine.
- The final GPU count includes completions drained during shutdown.
- Peak thermal state and first serious observation follow the documented
  semantics.
- Reporting uses only public APIs and does not reduce the requested load.
- Duration, signal, and GPU-failure cleanup remain coordinated.
- All tests and strict builds pass on supported macOS systems.
