# Corekiln GPU Modes Design

Date: 2026-07-30

## Context

Corekiln currently creates sustained CPU demand by running one native worker
per active logical CPU. It needs an equivalent GPU workload and explicit
resource modes so a user can load the CPU, GPU, or both.

The current machine has a 20-core Apple M4 Pro GPU with Metal 4 support. The
implementation should also work on other Metal-capable Macs without private
APIs, elevated privileges, device-specific binaries, or attempts to bypass
macOS thermal management.

## Decision

Add a Metal compute engine alongside the existing CPU engine.

Corekiln will default to loading both CPU and GPU. Explicit mode flags select a
single engine or restate the default:

```text
corekiln [--cpu | --gpu | --both] [--workers N] [--duration SECONDS]
```

- no mode flag: CPU and GPU
- `--cpu`: CPU only
- `--gpu`: GPU only
- `--both`: CPU and GPU

At most one mode flag may appear. Duplicate or conflicting mode flags fail
before either engine starts. `--workers N` controls the CPU engine and is
invalid with `--gpu`. Existing positive-integer validation, `--duration`,
`--help`, `SIGINT`, and `SIGTERM` behavior remain.

## Alternatives Considered

### Precompiled Metal Library

Store the compute kernel in a `.metal` file and compile a `.metallib` during
the wrapper build.

This reduces shader startup time and catches shader errors during the native
build. It also adds a second compiler pipeline and a generated artifact. The
`metallib` command-line tool is unavailable in the current environment even
though the Metal compiler is present, so this would make normal builds less
reliable.

### Offscreen Render Pipeline

Continuously render into an offscreen texture.

This exercises graphics-specific stages, but it needs render targets, vertex
and fragment shaders, and more state than the load generator requires. It also
makes saturation dependent on raster work rather than directly issuing
parallel computation.

### Runtime-Compiled Compute Pipeline

Compile an embedded Metal Shading Language compute kernel at startup, create
one compute pipeline, and keep multiple command buffers in flight.

This uses the supported Metal compute path, requires no generated shader
artifact, and keeps GPU code isolated behind a C interface. The one-time
startup compile is acceptable for a long-running stress utility. This is the
selected approach.

Apple references:

- [Performing calculations on a GPU](https://developer.apple.com/documentation/Metal/performing-calculations-on-a-gpu)
- [Metal libraries](https://developer.apple.com/documentation/metal/metal-libraries?language=objc)
- [MTLCommandQueue](https://developer.apple.com/documentation/Metal/MTLCommandQueue)
- [MTLCommandBuffer](https://developer.apple.com/documentation/metal/mtlcommandbuffer?language=objc)

## Components

### `src/corekiln.c`

The executable entrypoint will own:

- command-line parsing and mode validation;
- active CPU discovery when a selected mode includes CPU;
- the kqueue containing signal, duration, and internal failure events;
- engine startup ordering;
- user-facing startup and shutdown output; and
- coordinated cleanup when setup or runtime work fails.

The entrypoint will create and validate every selected engine before starting
load. If GPU preparation fails in `--both` mode, no CPU worker will remain
running.

### `src/cpu_kiln.h` and `src/cpu_kiln.c`

The existing CPU worker implementation will move behind a lifecycle API:

1. create and allocate state;
2. start the requested pthread workers;
3. request stop;
4. join workers and destroy state.

The engine owns its atomic stop flag, thread array, worker contexts, and error
reporting. The arithmetic loop remains unchanged.

### `src/gpu_kiln.h` and `src/gpu_kiln.m`

The GPU engine will expose a plain C lifecycle API while its implementation
uses Objective-C and Metal:

1. obtain the system default Metal device;
2. compile an embedded compute kernel into a Metal library;
3. create the compute pipeline, command queue, and private output buffers;
4. start one submission pthread;
5. stop submission, drain in-flight buffers, and destroy all resources.

Preparation and submission are separate. Metal device, shader, pipeline, and
buffer failures happen before load starts. The submission thread starts only
after every selected engine is prepared.

The GPU engine retains a copied UTF-8 device name for startup output.

## GPU Workload

The embedded compute kernel will:

- dispatch a grid of 262,144 GPU threads;
- run 512 iterations of dependent nonlinear `float4` arithmetic per thread;
- derive each thread's initial state from its grid index; and
- write the final state to a private Metal buffer.

The dependency chain and observable buffer write prevent the shader from being
optimized away. The dispatch is intentionally finite so macOS can schedule
other GPU work and apply thermal controls.

The submission thread maintains three reusable slots. Each slot has its own
private output buffer and completion semaphore. For every available slot, the
thread creates a command buffer, encodes the compute pipeline, commits it, and
uses a completion handler to release that slot. Keeping three commands in
flight avoids a CPU-side wait gap between finite dispatches.

Threadgroup width comes from the compute pipeline's supported maximum and is
clamped to the grid size. Corekiln does not assume a particular Apple GPU
family or core count.

At shutdown, the engine stops submitting new commands, waits for all committed
commands to complete, and checks command-buffer status. A successful GPU run
must complete at least one command buffer.

## Lifecycle and Failure Flow

1. Parse and validate all arguments.
2. Create the kqueue and register `SIGINT`, `SIGTERM`, the optional timer, and
   an internal `EVFILT_USER` failure event.
3. Prepare the selected CPU and GPU engines without starting load.
4. Start every selected engine.
5. Print the selected mode, CPU worker count where applicable, GPU name where
   applicable, and stop condition.
6. Wait for a signal, duration expiry, or internal engine failure.
7. Request both engines to stop.
8. Join CPU workers, drain GPU commands, and destroy both engines.
9. Exit successfully only if all selected engines ran and stopped cleanly.

If the GPU submission thread detects command-buffer failure, it stores a
stable error message under its state mutex and then triggers the internal
kqueue user event. This ordering lets the main thread read the failure safely
after it wakes. An otherwise unbounded run therefore stops the CPU engine and
exits nonzero instead of silently continuing in CPU-only mode.

Startup output examples:

```text
corekiln: burning CPU (14 workers) + GPU (Apple M4 Pro) until interrupted
corekiln: burning CPU (14 workers) for 10 seconds
corekiln: burning GPU (Apple M4 Pro) for 10 seconds
```

Successful shutdown remains:

```text
corekiln: stopped
```

## Build

`bin/corekiln` will rebuild when any source or header is newer than the
generated binary. One Apple Clang invocation will compile the C and
Objective-C sources and link:

- POSIX threads;
- Foundation;
- Metal; and
- Objective-C blocks support.

The generated executable remains `.build/corekiln`. Runtime shader compilation
means the wrapper does not need `metal`, `metallib`, or an Xcode project.

## Test Coverage

The Minitest suite will continue exercising the real executable and will add:

- help output for all three mode flags;
- default mode selection as CPU and GPU;
- explicit `--cpu`, `--gpu`, and `--both` timed runs;
- rejection of duplicate or conflicting modes;
- rejection of `--workers` with GPU-only mode;
- actual Metal device, shader, pipeline, dispatch, and completion on the local
  GPU;
- `SIGINT` cleanup in combined mode;
- wrapper rebuilding and argument forwarding with every new source file; and
- strict warning-free compilation of C and Objective-C sources.

GPU tests use one-second durations. CPU tests use one worker unless the test
specifically covers active-CPU discovery. Tests never start an unbounded load
without guaranteed process cleanup.

Final verification will run CPU-only, GPU-only, and combined timed smoke tests
on the local M4 Pro. Exact utilization percentages remain observational because
macOS owns scheduling and telemetry.

## Safety Boundary

Combined mode intentionally raises CPU and GPU power demand at the same time,
which can produce more heat and faster battery drain than the existing
CPU-only version. Corekiln will document that behavior prominently.

Corekiln does not change fan policy, power limits, process priority, GPU
selection policy, thermal limits, or shutdown behavior. It creates no daemon,
launch agent, login item, or restart mechanism. `SIGKILL`, process termination,
or an operating-system shutdown still ends the load.

The tool guarantees continuously available CPU work and a continuously
occupied Metal compute queue. It does not guarantee a particular clock speed
or an exact utilization number after macOS scheduling and throttling.
