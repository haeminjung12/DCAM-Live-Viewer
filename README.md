# DCAM Live Viewer (Hamamatsu)

Qt desktop application for live viewing and capture control of Hamamatsu cameras via the DCAM SDK.

## What it does
- Opens and streams a live camera feed with zoom/pan viewing
- Shows real-time stats (resolution, FPS, dropped frames, readout speed)
- Lets you set resolution presets or custom sizes, binning, exposure, and readout speed
- Records frames to disk as timestamped TIFF sequences with progress feedback
- Writes session logs to `session_log.txt` (rotated/pruned automatically)

Tested with Hamamatsu ORCA-Fusion C14440
