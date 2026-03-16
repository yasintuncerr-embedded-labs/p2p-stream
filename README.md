# p2p-stream

Low-latency peer-to-peer video streaming stack for embedded devices (especially NXP) with GStreamer pipelines, runtime control via FIFO, and profile-based configuration.

This repository now includes:
- Production hardening refactors for runtime stability.
- Device-independent smoke testing on PC.
- Ready-to-run sender/receiver scenarios for NXP, macOS, Linux, and Windows.

## Setup

### 1) Prerequisites

If you see `cmake: command not found`, install prerequisites first.

Debian/Ubuntu (including Raspberry Pi OS 64-bit):

```bash
sudo apt update
sudo apt install -y \
  cmake pkg-config build-essential gdb \
  libglib2.0-dev \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav \
  libgstrtspserver-1.0-dev
```

macOS (Homebrew):

```bash
brew update
brew install cmake pkg-config glib gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-plugins-ugly libav
```

Quick tool checks:

```bash
cmake --version
gcc --version
gdb --version
pkg-config --modversion gstreamer-1.0 glib-2.0
gst-launch-1.0 --version
```

### 2) Build p2p-stream (Linux/macOS)

```bash
cmake -S . -B build
cmake --build build -j
```

Binary path after build:

```bash
./build/p2p-stream
```

### 3) Profiles

Default profile directory on target systems:

```bash
/etc/p2p-stream/device-profiles
```

Local development profile directory:

```bash
./device-profiles
```

### 4) Quick local validation (device-independent)

```bash
ctest --test-dir build --output-on-failure -R pc_smoke
```

Detailed test documentation:

- [PC Smoke Tests](tests/README.md)

## Example Scenarios

Set network variables for your current environment:

```bash
export SENDER_IP="<sender_ip_on_current_network>"
export RECEIVER_IP="<receiver_ip_on_current_network>"
```

## A) NXP sender -> macOS receiver

Run sender on NXP:

```bash
p2p-stream --net-role sta --role sender \
  --device nxp --codec h264 \
  --trigger auto \
  --width 1920 --height 1080 --fps 60 \
  --peer-ip "${RECEIVER_IP}" \
  --profiles /etc/p2p-stream/device-profiles \
  --verbose
```

Run receiver on macOS:

```bash
gst-launch-1.0 -v \
  udpsrc port=5600 buffer-size=8388608 do-timestamp=true \
  caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=97" \
  ! rtph264depay \
  ! h264parse \
  ! avdec_h264 \
  ! videoconvert \
  ! autovideosink sync=false
```

## B) macOS sender (iPhone camera) -> NXP receiver

Run receiver on NXP:

```bash
p2p-stream --net-role sta --role receiver \
  --device nxp --codec h264 \
  --trigger auto \
  --peer-ip "${SENDER_IP}" \
  --profiles /etc/p2p-stream/device-profiles \
  --verbose
```

Run sender on macOS (iPhone continuity camera example):

```bash
gst-launch-1.0 -v \
  avfvideosrc device-index=1 ! \
  video/x-raw,width=1280,height=720,format=NV12,framerate=30/1 ! \
  videoconvert ! \
  x264enc tune=zerolatency bitrate=4000 speed-preset=ultrafast pass=cbr key-int-max=30 ! \
  h264parse config-interval=-1 ! \
  rtph264pay config-interval=1 pt=97 mtu=1316 timestamp-offset=0 ! \
  udpsink host="${RECEIVER_IP}" port=5600 sync=false
```

Note:
- Confirm `device-index` with `gst-device-monitor-1.0 Video/Source`.
- Use `SENDER_IP` and `RECEIVER_IP` based on your current network.

## C) NXP sender -> Linux receiver

Run sender on NXP (p2p-stream):

```bash
p2p-stream --net-role sta --role sender \
  --device nxp --codec h264 \
  --trigger auto \
  --peer-ip "${RECEIVER_IP}" \
  --profiles /etc/p2p-stream/device-profiles \
  --verbose
```

Run receiver on Linux (GStreamer):

```bash
gst-launch-1.0 -v \
  udpsrc port=5600 buffer-size=8388608 do-timestamp=true \
  caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=97" ! \
  rtpjitterbuffer latency=50 drop-on-latency=true ! \
  rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! autovideosink sync=false
```

## D) Linux sender -> NXP receiver

Run receiver on NXP (p2p-stream):

```bash
p2p-stream --net-role sta --role receiver \
  --device nxp --codec h264 \
  --trigger auto \
  --peer-ip "${SENDER_IP}" \
  --profiles /etc/p2p-stream/device-profiles \
  --verbose
```

Run sender on Linux (GStreamer, webcam):

```bash
gst-launch-1.0 -v \
  v4l2src device=/dev/video0 ! \
  video/x-raw,width=1280,height=720,framerate=30/1 ! \
  videoconvert ! \
  x264enc tune=zerolatency bitrate=4000 speed-preset=ultrafast key-int-max=30 ! \
  h264parse config-interval=-1 ! \
  rtph264pay config-interval=1 pt=97 mtu=1316 ! \
  udpsink host="${RECEIVER_IP}" port=5600 sync=false async=false
```

## E) NXP sender -> Windows receiver

Run sender on NXP (p2p-stream):

```bash
p2p-stream --net-role sta --role sender \
  --device nxp --codec h264 \
  --trigger auto \
  --peer-ip "${RECEIVER_IP}" \
  --profiles /etc/p2p-stream/device-profiles \
  --verbose
```

Run receiver on Windows (PowerShell, GStreamer):

```powershell
gst-launch-1.0 -v `
  udpsrc port=5600 buffer-size=8388608 do-timestamp=true `
  caps="application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=97" ! `
  rtpjitterbuffer latency=50 drop-on-latency=true ! `
  rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! d3d11videosink sync=false
```

## F) Windows sender -> NXP receiver

Run receiver on NXP (p2p-stream):

```bash
p2p-stream --net-role sta --role receiver \
  --device nxp --codec h264 \
  --trigger auto \
  --peer-ip "${SENDER_IP}" \
  --profiles /etc/p2p-stream/device-profiles \
  --verbose
```

Run sender on Windows (PowerShell, GStreamer):

```powershell
gst-launch-1.0 -v `
  ksvideosrc device-index=0 ! `
  video/x-raw,width=1280,height=720,framerate=30/1 ! `
  videoconvert ! `
  x264enc tune=zerolatency bitrate=4000 speed-preset=ultrafast key-int-max=30 ! `
  h264parse config-interval=-1 ! `
  rtph264pay config-interval=1 pt=97 mtu=1316 ! `
  udpsink host="${RECEIVER_IP}" port=5600 sync=false async=false
```

## GStreamer differences by platform

Pipeline logic is mostly the same (`H264 -> RTP -> UDP`). Platform differences are mainly source/sink elements:

- macOS sender source: `avfvideosrc`
- Linux sender source: `v4l2src`
- Windows sender source: `ksvideosrc`
- macOS/Linux receiver sink: `autovideosink`
- Windows receiver sink: `d3d11videosink`

## Troubleshooting

- If receiver shows no video, verify payload type and caps (`pt=97`, `encoding-name=H264`) on both sides.
- If latency grows, reduce resolution/fps or tune `jitterbuffer latency`.
- If camera source fails, test source alone first with `gst-launch-1.0 <source> ! autovideosink`.
