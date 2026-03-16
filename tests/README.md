# PC Smoke Tests

These tests provide fast, device-independent validation on a development PC before running on NXP or other target hardware.

## Test Map

- Test script: `tests/pc_smoke.sh`
- CTest registration: `CMakeLists.txt` (test name: `pc_smoke`)
- CI workflow: `.github/workflows/pc-smoke.yml`
- Default artifacts path: `build/test-artifacts/pc-smoke`

## Why this exists

The goal is to catch control-plane and lifecycle regressions early, so deployment debugging on real devices is reduced.

This smoke test targets:
- startup and shutdown reliability
- CLI validation behavior
- FIFO command path behavior
- state transitions (`IDLE` <-> `STREAMING`)
- event storm tolerance (`start`/`stop` bursts)

## Scope and Coverage

Covered:
- CLI negative checks: invalid `--device`, invalid `--width`
- process bootstrap in manual mode
- FIFO and status file creation
- start/stop transition via control FIFO
- command storm handling
- graceful process termination

Not covered:
- real camera device integration
- hardware encoder/decoder paths
- long-run burn-in and memory leak profiling
- network quality edge cases (packet loss simulation)

## Prerequisites

- project is built (`build/p2p-stream` exists)
- `gst-inspect-1.0` is available
- required plugins are installed:
	- `videotestsrc`
	- `x264enc`
	- `h264parse`
	- `rtph264pay`
	- `udpsink`

## Execution Modes

Direct script execution:

```bash
chmod +x tests/pc_smoke.sh
./tests/pc_smoke.sh ./build/p2p-stream ./device-profiles ./build/test-artifacts/pc-smoke
```

CTest execution:

```bash
cd build
ctest --output-on-failure -R pc_smoke
```

## What the script does step-by-step

1. Validates binary path and required tools/plugins.
2. Runs negative CLI checks that must fail.
3. Starts `p2p-stream` in sender/manual mode with test pattern (`videotestsrc`).
4. Waits for control FIFO and status file creation.
5. Sends `start` and verifies `STREAMING` state in status JSON.
6. Sends `stop` and verifies `IDLE` state in status JSON.
7. Sends repeated `start/stop` burst commands.
8. Verifies process is still alive after storm.
9. Terminates process and exits with `[PASS] pc_smoke`.

## Expected Results

Success indicators:
- CTest output includes `1/1 Test #1: pc_smoke ... Passed`.
- Script output ends with `[PASS] pc_smoke`.

Failure indicators:
- plugin missing errors such as `[FAIL] Missing GStreamer plugin: ...`
- FIFO/status timeout errors
- unexpected process exit during command storm

## Artifacts and Logs

By default, test files are created under:

```text
build/test-artifacts/pc-smoke/
```

Important files:
- `app.log`: application file logger output
- `stdout.log`: process stdout/stderr capture
- `run/p2p-stream.cmd`: FIFO used by test
- `run/p2p-stream.status`: JSON state file sampled by assertions

## CI Behavior

The workflow `.github/workflows/pc-smoke.yml` runs on push/PR and performs:

1. dependency install (build tools + GStreamer)
2. configure + build
3. `ctest --output-on-failure -R pc_smoke`
4. artifact upload from `build/test-artifacts/pc-smoke`

## Troubleshooting

If the test fails:
- check plugin availability:

```bash
gst-inspect-1.0 videotestsrc x264enc h264parse rtph264pay udpsink
```

- inspect logs:

```bash
cat build/test-artifacts/pc-smoke/app.log
cat build/test-artifacts/pc-smoke/stdout.log
```

- rerun only smoke test with verbose output:

```bash
ctest --test-dir build --output-on-failure -R pc_smoke
```

## Extending this test

Recommended next additions:
- receiver-mode smoke case
- H265 software path smoke case
- long-run stability mode (for example 5-10 minutes)
- optional packet-loss/jitter simulation profile
