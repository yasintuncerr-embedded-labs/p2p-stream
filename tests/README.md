# PC Smoke Tests

These tests are designed for device-independent validation on a development PC.

## What is covered

- CLI input validation failures (invalid device selector and invalid numeric input)
- End-to-end process bootstrap
- FIFO control path creation and status file updates
- Manual `start`/`stop` lifecycle transition
- Command storm robustness (`start`/`stop` bursts)

## Prerequisites

- Project is built (`build/p2p-stream` exists)
- GStreamer tools are installed (`gst-inspect-1.0`)
- Required plugins: `videotestsrc`, `x264enc`, `h264parse`, `rtph264pay`, `udpsink`

## Run directly

```bash
chmod +x tests/pc_smoke.sh
./tests/pc_smoke.sh ./build/p2p-stream ./device-profiles ./build/test-artifacts/pc-smoke
```

## Run via CTest

```bash
cd build
ctest --output-on-failure -R pc_smoke
```

Artifacts are written under `build/test-artifacts/pc-smoke` by default.

## CI

GitHub Actions workflow at `.github/workflows/pc-smoke.yml` runs this test on push/PR.

It performs:

- dependency install (GStreamer + build tools)
- configure/build
- `ctest --output-on-failure -R pc_smoke`
- artifact upload from `build/test-artifacts/pc-smoke`
