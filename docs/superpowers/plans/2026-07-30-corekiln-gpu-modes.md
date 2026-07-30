# Corekiln GPU Modes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a real Metal compute workload and selectable CPU, GPU, and
combined modes, with combined load as Corekiln's default.

**Architecture:** Keep argument parsing and kqueue orchestration in
`src/corekiln.c`. Move CPU threads behind a C lifecycle API and implement the
GPU engine in Objective-C behind an equivalent C API; both engines prepare,
start, receive a coordinated stop request, and join before destruction.

**Tech Stack:** C11, Objective-C ARC, POSIX threads, macOS kqueue, Metal,
Foundation, Objective-C blocks, Ruby Minitest, Apple Clang

---

## File Structure

- `src/corekiln.c` — CLI modes, CPU discovery, kqueue events, lifecycle
  orchestration, and output.
- `src/cpu_kiln.h` — opaque CPU engine interface.
- `src/cpu_kiln.c` — CPU thread allocation, arithmetic workload, stop, and join.
- `src/gpu_kiln.h` — opaque GPU engine interface usable from C.
- `src/gpu_kiln.m` — Metal device, runtime shader, compute queue, in-flight
  slots, failure notification, and GPU submission thread.
- `bin/corekiln` — rebuild detection for every source/header and framework
  linking.
- `test/corekiln_test.rb` — black-box CLI, lifecycle, real Metal, and wrapper
  tests.
- `README.md` — modes, examples, telemetry expectations, and combined-load
  caution.

### Task 1: Add modes and isolate the CPU engine

**Files:**

- Create: `src/cpu_kiln.h`
- Create: `src/cpu_kiln.c`
- Modify: `src/corekiln.c`
- Modify: `bin/corekiln`
- Modify: `test/corekiln_test.rb`

- [ ] **Step 1: Write failing mode and CPU-only tests**

Update the help expectations:

```ruby
assert_includes stdout, "--cpu"
assert_includes stdout, "--gpu"
assert_includes stdout, "--both"
assert_includes stdout, "Default: --both"
```

Add:

```ruby
def test_rejects_duplicate_and_conflicting_modes
  with_compiled_corekiln do |binary|
    [
      %w[--cpu --cpu],
      %w[--cpu --gpu],
      %w[--gpu --both],
      %w[--both --cpu],
    ].each do |arguments|
      _stdout, stderr, status = Open3.capture3(binary, *arguments)

      refute status.success?, "Expected #{arguments.inspect} to fail"
      assert_includes stderr, "only one mode may be specified"
    end
  end
end

def test_rejects_workers_in_gpu_only_mode
  with_compiled_corekiln do |binary|
    _stdout, stderr, status = Open3.capture3(
      binary,
      "--gpu",
      "--workers",
      "1",
      "--duration",
      "1",
    )

    refute status.success?
    assert_includes stderr, "--workers requires a CPU mode"
  end
end

def test_cpu_mode_runs_requested_workers
  with_compiled_corekiln do |binary|
    stdout, stderr, status = Open3.capture3(
      binary,
      "--cpu",
      "--workers",
      "1",
      "--duration",
      "1",
    )

    assert status.success?, stderr
    assert_includes stdout, "corekiln: burning CPU (1 worker) for 1 second"
    assert_includes stdout, "corekiln: stopped"
  end
end
```

Change the existing timed and interrupt tests to pass `--cpu` and expect the
CPU-specific startup line.

- [ ] **Step 2: Run the focused tests and verify RED**

Run:

```sh
ruby test/corekiln_test.rb --name '/mode|cpu|help|interrupt/'
```

Expected: FAIL because the current parser does not recognize mode flags and the
startup output has no engine label.

- [ ] **Step 3: Define the CPU lifecycle API**

Create `src/cpu_kiln.h`:

```c
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
```

Move the existing `worker_context`, atomic stop flag, arithmetic loop, thread
creation, partial-start cleanup, and joins into `src/cpu_kiln.c`. Make the stop
flag an instance member so lifecycle state is owned by `cpu_kiln`:

```c
struct cpu_kiln {
  size_t worker_count;
  size_t started_count;
  pthread_t *threads;
  worker_context *contexts;
  atomic_bool stop_requested;
  bool joined;
};
```

Use `snprintf(error, error_size, ...)` for allocation, `pthread_create`, and
`pthread_join` failures. A failed start must request stop, join all threads
already created, and return `false` with no live worker.

- [ ] **Step 4: Add mode parsing and CPU-only orchestration**

Define:

```c
typedef enum {
  KILN_MODE_BOTH,
  KILN_MODE_CPU,
  KILN_MODE_GPU,
} kiln_mode;

typedef struct {
  kiln_mode mode;
  bool mode_set;
  size_t worker_count;
  unsigned int duration_seconds;
  bool worker_count_set;
  bool duration_set;
} options;
```

Parse `--cpu`, `--gpu`, and `--both` through one helper. Reject a second mode
flag, even if it repeats the first. After parsing, reject `--workers` when
`mode == KILN_MODE_GPU`.

For this task, run the extracted CPU engine for `--cpu`. Return a clear nonzero
`corekiln: GPU engine is unavailable` error for GPU-containing modes until the
next failing test introduces the Metal engine.

Preserve the kqueue signal and timer implementation for CPU-only mode, but arm
the timer after `cpu_kiln_start`.

- [ ] **Step 5: Update direct and wrapper builds**

Change the test compiler source list to:

```ruby
SOURCES = %w[
  src/corekiln.c
  src/cpu_kiln.c
].map { |path| File.join(ROOT, path) }.freeze
```

Pass `*SOURCES` to Clang. Change the wrapper to rebuild when any file in this
list is newer than the binary:

```sh
sources="
$repository_dir/src/corekiln.c
$repository_dir/src/cpu_kiln.c
$repository_dir/src/cpu_kiln.h
"

rebuild=false
for source_file in $sources; do
  if [ ! -x "$binary" ] || [ "$source_file" -nt "$binary" ]; then
    rebuild=true
    break
  fi
done
```

Compile both C sources. Update the wrapper test to copy the entire `src`
directory into its temporary repository.

- [ ] **Step 6: Run all tests and verify GREEN**

Run:

```sh
ruby test/corekiln_test.rb
```

Expected: all CPU, parser, signal, and wrapper tests pass with strict warnings.

- [ ] **Step 7: Commit the CPU engine and modes**

```sh
git add bin/corekiln src/corekiln.c src/cpu_kiln.c src/cpu_kiln.h \
  test/corekiln_test.rb
git commit -m "Add Corekiln resource modes"
```

### Task 2: Add the Metal GPU engine

**Files:**

- Create: `src/gpu_kiln.h`
- Create: `src/gpu_kiln.m`
- Modify: `src/corekiln.c`
- Modify: `bin/corekiln`
- Modify: `test/corekiln_test.rb`

- [ ] **Step 1: Write the failing real-GPU test**

Add:

```ruby
def test_gpu_mode_runs_real_metal_work
  with_compiled_corekiln do |binary|
    stdout, stderr, status = Open3.capture3(
      binary,
      "--gpu",
      "--duration",
      "1",
    )

    assert status.success?, stderr
    assert_match(/corekiln: burning GPU \(.+\) for 1 second/, stdout)
    assert_includes stdout, "corekiln: stopped"
    assert_empty stderr
  end
end
```

- [ ] **Step 2: Run the GPU test and verify RED**

Run:

```sh
ruby test/corekiln_test.rb --name test_gpu_mode_runs_real_metal_work
```

Expected: FAIL with `corekiln: GPU engine is unavailable`.

- [ ] **Step 3: Define the GPU lifecycle API**

Create `src/gpu_kiln.h`:

```c
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
const char *gpu_kiln_device_name(const gpu_kiln *kiln);

#endif
```

- [ ] **Step 4: Implement Metal preparation**

Create `src/gpu_kiln.m` with an ARC-managed `CorekilnGPUState` object retained
behind the opaque C pointer. Its initializer must:

```objc
_device = MTLCreateSystemDefaultDevice();
_library = [_device newLibraryWithSource:CorekilnShaderSource
                                  options:nil
                                    error:&metalError];
id<MTLFunction> function = [_library newFunctionWithName:@"corekiln_burn"];
_pipeline = [_device newComputePipelineStateWithFunction:function
                                                    error:&metalError];
_commandQueue = [_device newCommandQueue];
```

Create three `MTLResourceStorageModePrivate` buffers, each sized for 262,144
`float4` outputs, and one semaphore per slot.

Use this embedded MSL kernel:

```metal
#include <metal_stdlib>
using namespace metal;

kernel void corekiln_burn(device float4 *output [[buffer(0)]],
                          uint gid [[thread_position_in_grid]]) {
  if (gid >= 262144) {
    return;
  }

  float index = float(gid + 1);
  float4 state = fract(index * float4(0.1031, 0.11369, 0.13787, 0.09987));
  for (uint iteration = 0; iteration < 512; iteration++) {
    state = fract(fma(state,
                      state.wxyz + float4(1.001, 1.003, 1.007, 1.009),
                      float4(0.101, 0.211, 0.307, 0.419)));
  }
  output[gid] = state;
}
```

Return the localized Metal error text through the caller's C error buffer. Do
not start a submission thread during preparation.

- [ ] **Step 5: Implement continuous command submission**

The GPU state owns:

```objc
id<MTLBuffer> _buffers[3];
dispatch_semaphore_t _slots[3];
pthread_t _thread;
atomic_bool _stopRequested;
atomic_uint _completedCommands;
BOOL _threadStarted;
NSLock *_failureLock;
NSString *_failureMessage;
int _stopQueue;
uintptr_t _failureEvent;
```

The pthread cycles through slots, waits for that slot's semaphore, creates a
command buffer and compute encoder, encodes the pipeline and slot buffer, then
dispatches enough threadgroups to cover 262,144 threads. Use
`maxTotalThreadsPerThreadgroup` for width and a ceiling division for group
count.

Attach a completion handler before commit:

```objc
[commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
  if (completed.status == MTLCommandBufferStatusCompleted) {
    atomic_fetch_add_explicit(&state->_completedCommands, 1,
                              memory_order_relaxed);
  } else {
    [state recordFailure:completed.error.localizedDescription];
  }
  dispatch_semaphore_signal(slotSemaphore);
}];
```

`recordFailure:` stores only the first failure under `_failureLock`, sets the
atomic stop flag, and triggers the registered `EVFILT_USER` event with
`NOTE_TRIGGER`.

When stop is requested, submit no new command. Drain all three slot semaphores
before the pthread returns. `gpu_kiln_join` fails if the submission thread,
command status, or zero-completion invariant fails.

- [ ] **Step 6: Integrate GPU-only orchestration**

Register the internal event:

```c
#define COREKILN_FAILURE_EVENT 1

EV_SET(&changes[change_count++], COREKILN_FAILURE_EVENT, EVFILT_USER,
       EV_ADD | EV_CLEAR, NOTE_FFNOP, 0, NULL);
```

For `--gpu`, prepare the GPU before building the stop queue, start it with the
queue and event identifier, arm the optional timer, print the device name, wait,
request stop, join, and destroy. If the returned event is the internal failure
event, print the GPU failure and exit nonzero.

- [ ] **Step 7: Update Clang and wrapper sources**

Compile with:

```ruby
COMPILER_FLAGS = %w[
  -std=c11 -O2 -Wall -Wextra -Werror -pthread -fobjc-arc -fblocks
].freeze
LINKER_FLAGS = ["-framework", "Foundation", "-framework", "Metal"].freeze
```

Add `src/gpu_kiln.m` to compiled sources and `src/gpu_kiln.h` to wrapper
freshness inputs. Use `xcrun clang` in tests and the wrapper.

- [ ] **Step 8: Run all tests and verify GREEN**

Run:

```sh
ruby test/corekiln_test.rb
```

Expected: the GPU test compiles the shader, submits real Metal work for one
second, completes at least one command, and exits cleanly.

- [ ] **Step 9: Commit the Metal engine**

```sh
git add bin/corekiln src/corekiln.c src/gpu_kiln.h src/gpu_kiln.m \
  test/corekiln_test.rb
git commit -m "Add Corekiln Metal workload"
```

### Task 3: Coordinate combined mode

**Files:**

- Modify: `src/corekiln.c`
- Modify: `test/corekiln_test.rb`

- [ ] **Step 1: Write failing default and explicit combined tests**

Add:

```ruby
def test_default_mode_runs_cpu_and_gpu
  with_compiled_corekiln do |binary|
    stdout, stderr, status = Open3.capture3(
      binary,
      "--workers",
      "1",
      "--duration",
      "1",
    )

    assert status.success?, stderr
    assert_match(
      /corekiln: burning CPU \(1 worker\) \+ GPU \(.+\) for 1 second/,
      stdout,
    )
  end
end

def test_explicit_both_mode_runs_cpu_and_gpu
  with_compiled_corekiln do |binary|
    stdout, stderr, status = Open3.capture3(
      binary,
      "--both",
      "--workers",
      "1",
      "--duration",
      "1",
    )

    assert status.success?, stderr
    assert_match(/CPU \(1 worker\) \+ GPU \(.+\)/, stdout)
    assert_includes stdout, "corekiln: stopped"
  end
end
```

Change the interrupt test to use `--both --workers 1` and expect the combined
startup line.

- [ ] **Step 2: Run combined tests and verify RED**

Run:

```sh
ruby test/corekiln_test.rb --name '/default_mode|explicit_both|interrupt/'
```

Expected: FAIL because CPU and GPU are not yet started in the same lifecycle.

- [ ] **Step 3: Implement prepare-start-stop-join ordering**

In `src/corekiln.c`, determine selected engines with:

```c
static bool mode_includes_cpu(kiln_mode mode) {
  return mode == KILN_MODE_CPU || mode == KILN_MODE_BOTH;
}

static bool mode_includes_gpu(kiln_mode mode) {
  return mode == KILN_MODE_GPU || mode == KILN_MODE_BOTH;
}
```

For every run:

1. prepare selected CPU and GPU engines;
2. create the stop queue;
3. start CPU, then GPU;
4. arm the timer only after both starts succeed;
5. print the selected engine names;
6. wait for signal, timer, or engine failure;
7. request stop from both engines before joining either;
8. join GPU and CPU, preserving the first failure;
9. destroy both states and close the queue.

Any failure after one engine starts must stop and join that engine before
returning. Default `options.mode` to `KILN_MODE_BOTH`.

- [ ] **Step 4: Run all tests and verify GREEN**

Run:

```sh
ruby test/corekiln_test.rb
```

Expected: CPU-only, GPU-only, default combined, explicit combined, interrupt,
validation, and wrapper tests all pass.

- [ ] **Step 5: Commit coordinated mode**

```sh
git add src/corekiln.c test/corekiln_test.rb
git commit -m "Coordinate combined CPU and GPU load"
```

### Task 4: Document, verify, merge, and publish

**Files:**

- Modify: `README.md`

- [ ] **Step 1: Update user documentation**

Change the opening to state that no arguments load both CPU and GPU. Document
`--cpu`, `--gpu`, `--both`, the CPU-only meaning of `--workers`, Metal and
Foundation linking, mode-specific examples, GPU History as observational
telemetry, and the increased heat/power impact of combined mode.

- [ ] **Step 2: Run the complete automated suite**

Run:

```sh
ruby test/corekiln_test.rb
```

Expected: zero failures and zero errors.

- [ ] **Step 3: Run strict direct compilation**

Run:

```sh
xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -pthread \
  -fobjc-arc -fblocks \
  src/corekiln.c src/cpu_kiln.c src/gpu_kiln.m \
  -framework Foundation -framework Metal \
  -o /tmp/corekiln-final-verification
```

Expected: exit 0 with no warnings.

- [ ] **Step 4: Run all three real-mode smoke tests**

Run:

```sh
bin/corekiln --cpu --workers 1 --duration 1
bin/corekiln --gpu --duration 3
bin/corekiln --both --workers 1 --duration 3
```

Expected: each mode reports the selected engines, runs for its requested
duration, completes real CPU or Metal work, and prints `corekiln: stopped`.

- [ ] **Step 5: Commit documentation**

```sh
git add README.md docs/superpowers/specs/2026-07-30-corekiln-gpu-modes-design.md \
  docs/superpowers/plans/2026-07-30-corekiln-gpu-modes.md
git commit -m "Document Corekiln GPU modes"
```

- [ ] **Step 6: Merge through Worktrunk and verify main**

From the implementation worktree:

```sh
wt merge main --no-squash --yes
```

Then run the full test suite and strict direct compilation from
`/Users/roman/code/corekiln`.

- [ ] **Step 7: Push and verify the remote**

Run:

```sh
git push origin main
git status --short --branch
git rev-parse HEAD
git ls-remote origin refs/heads/main
```

Expected: `main` tracks `origin/main`, the worktree is clean, and local and
remote hashes match.
