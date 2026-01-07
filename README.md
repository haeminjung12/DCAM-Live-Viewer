# DCAM Live Viewer (Hamamatsu)

Qt desktop application for live viewing, capture control, and offline review of Hamamatsu cameras via the DCAM SDK.

## What it does
- Streams a live camera feed with zoom/pan and scrollbars
- Shows real-time stats (resolution, FPS, dropped frames, readout speed)
- Lets you set resolution presets or custom sizes, binning (incl. independent), exposure (ms), bit depth (8/12/16), and readout speed
- Records frames to disk as timestamped TIFF sequences with a save progress dialog
- Captures a single frame to TIFF on demand
- Writes capture metadata to `capture_info.txt` (fps, resolution, exposure, etc.)
- Logs to `session_log.txt` (cleared on startup)
- Viewer window for saved sequences with slider, recent folders, and keyboard navigation
- Viewer-only mode if the camera fails to initialize at startup

Tested with Hamamatsu ORCA-Fusion C14440

## Viewer controls
- Left/Right: previous/next frame
- Ctrl+Left/Right: jump 5 frames
- PageUp/PageDown: jump 10 frames

## Build
- Qt: `C:\Qt\6.10.1\msvc2022_64`
- DCAM SDK headers/libs are expected in `Hamamatsu_DCAMSDK4_v25056964`
- Build with MSBuild (Release)

## Standalone package
Use `package_portable.ps1` to build and deploy Qt runtimes into `build/Release`.
The portable package relies on the system DCAM runtime (no bundled `dcamapi.dll`).
