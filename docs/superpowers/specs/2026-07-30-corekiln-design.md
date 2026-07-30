# Corekiln Design

Date: 2026-07-30

## Context

Corekiln is a small macOS command-line utility for deliberately keeping every
logical processor busy until the process is stopped. macOS remains responsible
for scheduling, frequency scaling, thermal throttling, and emergency shutdown.

The default invocation must be convenient and unbounded. Controlled worker
counts and timed runs are also required so behavior can be tested without
leaving an accidental background load.

## Decision

Implement a native C program using POSIX threads. Start one continuously
runnable worker per active logical CPU and rely on the macOS scheduler to
distribute those workers across the available cores.

Use macOS kqueue events to wait for `SIGINT`, `SIGTERM`, or an optional
one-shot duration. Do not use private Apple APIs, elevated privileges,
thread-affinity hacks, or thermal-management controls.

## Name

The repository and executable are named `corekiln`.

The name is short, pronounceable, and honest: “core” identifies the resource
being loaded, while “kiln” suggests sustained controlled heat. Exact-name
checks found no GitHub repository or Homebrew formula collision at the time of
creation.

## Components

### `src/corekiln.c`

The native program will:

- discover the active logical CPU count with `sysctlbyname("hw.activecpu")`,
  falling back to `sysconf(_SC_NPROCESSORS_ONLN)` if necessary;
- accept `--workers N` to override the detected worker count;
- accept `--duration SECONDS` for a positive, whole-number timed run;
- display usage with `--help`;
- run indefinitely when `--duration` is absent;
- create one pthread per worker;
- execute batches of integer arithmetic with an observable per-thread sink so
  the optimizer cannot remove the workload;
- stop all workers through a C11 atomic flag;
- wait through kqueue for `SIGINT`, `SIGTERM`, or the optional timer;
- join every worker before exiting; and
- report invalid arguments and operating-system failures on standard error.

The program will not pin workers to particular cores. macOS is the authority on
placement and may migrate threads as thermal and power conditions change.

### `bin/corekiln`

The executable wrapper will compile `src/corekiln.c` into the ignored
`.build/corekiln` binary when the binary is missing or older than the source.
It will use the system C compiler with C11, optimization, pthread support, and
strict warnings, then replace itself with the compiled program while forwarding
all arguments.

The normal interface is:

```sh
bin/corekiln
```

The process runs until interrupted with `Ctrl-C`. Controlled examples:

```sh
bin/corekiln --workers 2
bin/corekiln --duration 10
```

## Runtime Flow

1. The wrapper checks whether the native binary needs compilation.
2. The C program validates arguments and discovers the default worker count.
3. It registers signal and optional timer events with kqueue.
4. It creates all workers and reports the active load.
5. Workers remain runnable while the main thread waits for an event.
6. The main thread requests shutdown and joins every worker.
7. The process reports a clean stop and exits successfully.

If allocation or thread creation fails, the program requests shutdown, joins
the workers already created, reports the operating-system error, and exits
nonzero. Invalid arguments fail before any worker starts.

## Safety Boundary

Corekiln intentionally creates sustained maximum CPU demand and will increase
temperature, power consumption, and fan activity where applicable. It does not
disable or bypass macOS thermal protections. CPU frequency may be throttled even
while utilization remains fully occupied, so Corekiln guarantees runnable load
rather than a particular clock speed or telemetry reading.

The only persistent artifact is the rebuildable binary under `.build/`.
Corekiln creates no launch agent, daemon, login item, or automatic restart
mechanism.

## Test Coverage

A standalone Minitest suite will compile the C source with strict warnings and
verify:

- help output;
- invalid worker, duration, missing-value, and unknown arguments;
- a controlled run starts the requested worker count and exits after its
  duration;
- `SIGINT` stops an otherwise unbounded run; and
- the wrapper builds and launches the native binary while forwarding arguments.

Tests use reduced worker counts and short lifetimes. They do not deliberately
saturate every processor.
