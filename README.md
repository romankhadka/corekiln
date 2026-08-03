# Corekiln

Corekiln is a tiny native macOS utility that continuously loads the CPU, GPU,
or both. With no options, it starts one worker per active logical CPU, keeps a
Metal compute queue saturated, and runs until you press `Ctrl-C`.

macOS remains in control of scheduling, frequency scaling, thermal throttling,
and emergency shutdown. Corekiln does not disable or bypass those protections.

## Requirements

- macOS
- Xcode Command Line Tools (`xcode-select --install`)

## Run

```sh
bin/corekiln
```

The first run compiles the native C executable to `.build/corekiln`. Later runs
reuse it until a source file or header changes. The build uses Clang with
Foundation and Metal; no separate package manager or build system is required.

Stop an unbounded run with `Ctrl-C`.

## Options

```text
Usage: corekiln [--cpu | --gpu | --both] [options]

Keep macOS compute resources continuously busy.

Modes:
  --cpu                Load CPU only
  --gpu                Load GPU only
  --both               Load CPU and GPU
                       Default: --both

Options:
  --workers N          Number of CPU worker threads
  --duration SECONDS   Stop after a positive whole number of seconds
  --status SECONDS     Print progress every positive whole number of seconds
  --help               Show this help
```

Mode flags are mutually exclusive. `--workers` applies to modes that include
the CPU and is rejected with `--gpu`.

Examples:

```sh
# Load every active logical CPU and the GPU until interrupted
bin/corekiln

# Load two logical CPUs until interrupted
bin/corekiln --cpu --workers 2

# Load only the GPU for ten seconds
bin/corekiln --gpu --duration 10

# Load every active logical CPU and the GPU for ten seconds
bin/corekiln --both --duration 10

# Also print progress every two seconds and a final run report
bin/corekiln --both --duration 10 --status 2
```

## Run reports

Pass `--status SECONDS` to print periodic progress and a final report after the
selected engines have stopped and drained their work:

```text
corekiln: status elapsed=5.0s thermal=nominal cpu_work_units=824193 gpu_dispatches=71
corekiln: report elapsed=60.2s stop=duration peak_thermal=serious serious_at=23.0s cpu_work_units=9912461 gpu_dispatches=847
```

One CPU work unit is one completed 4,096-iteration arithmetic batch. One GPU
dispatch is one successfully completed Metal command buffer. These counters
show that the selected engines are making forward progress; they are not
utilization percentages or benchmark scores.

Thermal pressure is the qualitative state reported by macOS: `nominal`,
`fair`, `serious`, or `critical` (`unknown` is used if no known state is
available). It is not a temperature reading. Corekiln uses only public APIs and
does not require elevated privileges. Without `--status`, output remains the
startup line followed by `corekiln: stopped`.

Activity Monitor reports CPU usage per logical CPU, so a process occupying
eight logical CPUs can appear near 800 percent. Frequency may fall after macOS
thermally throttles the processor even while Corekiln keeps every worker
runnable.

Activity Monitor's GPU History window can be used to observe GPU load. Corekiln
continuously submits a compute-heavy Metal shader through three in-flight
command slots, but macOS still controls GPU scheduling and may reserve capacity
for the display, other applications, or thermal management.

## Caution

Corekiln intentionally creates sustained maximum CPU and GPU demand. Combined
mode produces substantially more heat and power draw than CPU-only mode. Expect
increased temperature, power consumption, fan activity where applicable, lower
battery life, and possible interface sluggishness. Save important work, connect
appropriate power, keep vents unobstructed, and stop the process if the machine
behaves unexpectedly.

Corekiln creates no daemon, launch agent, login item, or restart mechanism.

## Test

```sh
ruby test/corekiln_test.rb
```

The black-box suite compiles the native executable with strict warnings and
exercises argument validation, all resource modes, timed shutdown, interrupt
handling, the wrapper build, and real Metal command completion.
