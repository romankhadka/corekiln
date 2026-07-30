# Corekiln

Corekiln is a tiny native macOS utility that keeps logical CPU cores
continuously busy. With no options, it starts one worker per active logical CPU
and runs until you press `Ctrl-C`.

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
reuse it until `src/corekiln.c` changes.

Stop an unbounded run with `Ctrl-C`.

## Options

```text
Usage: corekiln [--workers N] [--duration SECONDS]

  --workers N          Number of worker threads
  --duration SECONDS   Stop after a positive whole number of seconds
  --help               Show help
```

Examples:

```sh
# Load two logical CPUs until interrupted
bin/corekiln --workers 2

# Load every active logical CPU for ten seconds
bin/corekiln --duration 10
```

Activity Monitor reports CPU usage per logical CPU, so a process occupying
eight logical CPUs can appear near 800 percent. Frequency may fall after macOS
thermally throttles the processor even while Corekiln keeps every worker
runnable.

## Caution

Corekiln intentionally creates sustained maximum CPU demand. Expect increased
temperature, power consumption, and fan activity where applicable. Save
important work, keep vents unobstructed, and stop the process if the machine
behaves unexpectedly.

Corekiln creates no daemon, launch agent, login item, or restart mechanism.
