# DroidSource

**DroidSource** is an OBS Studio source for Android screens and cameras, powered by [scrcpy](https://github.com/Genymobile/scrcpy). It started as a fork of [wtarit/scrcpy-obs](https://github.com/wtarit/scrcpy-obs) and has since grown into a much more camera-focused implementation with native Camera2 controls, advanced color management, 10-bit/HDR workflows, and stream-recovery features.

> **Status:** beta  
> CI builds are provided for Windows, macOS and Ubuntu. The macOS CI build uses the Apple Silicon scrcpy binary currently published by the companion [wtarit/scrcpy](https://github.com/wtarit/scrcpy) fork.

## Requirements

- **Android device** with USB debugging enabled.
- **OBS Studio 32.x** is recommended.
- A USB ADB connection to the Android device.

## Install

Download the appropriate build from [Releases](https://github.com/DLC86/DroidSource/releases), install it, and restart OBS.

The source appears as **DroidSource** in **Add Source**.

The CI also produces standalone plugin archives for development/testing.

## Usage

1. Connect the Android device and approve the USB debugging authorization.
2. In OBS, click **+ → DroidSource**.
3. Select the Android device and choose **Display** or **Camera**.
4. For camera mode, use **Refresh Cameras** to populate the capabilities exposed by the selected device.

Multiple DroidSource instances can be used for multiple Android devices.

## Features

The original [wtarit/scrcpy-obs](https://github.com/wtarit/scrcpy-obs) fork provided the basic OBS source, device selection, Display/Camera input selection, camera ID, resolution cap, bitrate and codec selection. DroidSource adds the following functionality on top of that project.

### Android camera controls

- Dynamic camera enumeration and **Refresh Cameras**, instead of a fixed 0–9 camera list.
- Device-reported **camera resolutions and frame rates** with automatic capability fallbacks.
- **Camera zoom** with device-aware limits and support for logical multi-camera switching.
- **Torch** control.
- **Manual ISO** with a range derived from the actual camera capabilities.
- Independent **shutter speed** control.
- **Manual focus distance** control.
- **White balance in Kelvin**.
- Real Camera2 **AWB lock** in Auto mode; selecting a manual Kelvin value disables the lock.
- Automatic handling of logical/physical camera changes so color-correction state is refreshed when the active physical lens changes.
- **Edge enhancement disabled** when supported.
- **Noise reduction disabled** when supported.
- Camera settings are applied through the Android **Camera2 request pipeline**, rather than by post-processing the final OBS image.

### Color management

- Selectable **Color Space / Gamma camera profiles** rather than only the camera default.
- sRGB, Rec.709 and Rec.2020 gamut choices where the device exposes them.
- Gamma / transfer choices including **2.2, 2.4, Rec.709 Scene, Rec.709-A, HLG, HDR10, HDR10+ and sRGB**, with the available list adapting to the camera capabilities.
- Camera2-native color correction and tone-mapping controls are used where the device supports the corresponding Camera2 capabilities.
- **Color range interpretation** can be set to Auto, Limited or Full.
- Camera color metadata is propagated to OBS for native 10-bit HDR paths.
- 8-bit output paths are explicitly advertised as SDR to OBS so profiles such as 8-bit sRGB/HLG are not accidentally treated as native HDR by OBS.

### 10-bit and HDR workflows

The camera bit-depth selector provides three modes:

| Mode | Pipeline |
|---|---|
| **8-bit** | Camera2 8-bit → OBS 8-bit |
| **10-bit** | Camera2 native 10-bit/HDR → OBS native 10-bit |
| **10-bit to 8-bit** | Camera2 native 10-bit → pure 10→8-bit reduction → OBS 8-bit SDR |

The **10-bit to 8-bit** path deliberately does not perform a Rec.2020→Rec.709 CST, HLG→SDR tone mapping, RGB conversion or other color transform on the PC. The selected Camera2 profile is retained before the bit-depth reduction, allowing an external LUT or other grading workflow to be applied later in OBS.

Native 10-bit output is kept separate from the 10-bit-to-8-bit path so HDR metadata and Camera2 HDR behavior are preserved for the native 10-bit mode.

### Stability and low-latency features

- Raw video is delivered directly to the plugin without requiring an intermediate scrcpy desktop window.
- Audio is disabled for the source.
- Configurable **camera buffering** presets including Automatic, 50 ms, 100 ms and 200 ms.
- Optional **hardware decoding** for camera mode.
- A watchdog detects stalled video streams and automatically restarts the scrcpy pipeline.
- TCP transport between the scrcpy process and OBS uses a local loopback connection with TCP_NODELAY.

## Source settings

| Setting | Description |
|---|---|
| **Device** | Android device from the ADB device list. |
| **Refresh Devices** | Refresh the ADB device list. |
| **Video source** | **Display** or **Camera**. |
| **Camera ID** | Camera exposed by the selected Android device. |
| **Refresh Cameras** | Re-query camera capabilities. |
| **Camera resolution** | Resolution supported by the selected camera. |
| **Camera FPS** | Frame rate supported by the selected camera. |
| **Camera zoom** | Camera2 zoom control. |
| **Torch** | Camera torch/flashlight control. |
| **ISO** | Manual camera sensitivity. |
| **Shutter** | Manual exposure time. |
| **Focus** | Manual focus distance. |
| **White balance** | Kelvin value; 0 keeps automatic white balance. |
| **White balance lock** | Locks Camera2 AWB while in Auto mode. |
| **Camera bit depth** | **8-bit**, **10-bit**, or **10-bit to 8-bit**. |
| **Color Space / Gamma** | Camera2 color-profile selection, subject to device support. |
| **Color range** | Auto, Limited or Full interpretation. |
| **Buffering** | Camera frame buffering: Automatic, 50 ms, 100 ms or 200 ms. |
| **Hardware decoding** | Use hardware video decoding when available. |
| **Max size** | Longest-edge cap for Display mode; 0 keeps native resolution. |
| **Bitrate** | Encoder bitrate. |
| **Video codec** | H.264, H.265 or AV1 when applicable. |

## Building from source

The project uses CMake and the same general OBS plugin build infrastructure as the original fork.

GitHub Actions currently build:

- **Windows x64**
- **macOS Universal plugin** (CI runner on Apple Silicon; bundled scrcpy is currently arm64)
- **Ubuntu 24.04 x86_64**

The Windows, macOS and Ubuntu reusable workflows also build the patched scrcpy-server used by DroidSource, so the Android Camera2 modifications are included in CI artifacts.

For developers and contributors: [CONTRIBUTING.md](CONTRIBUTING.md).

## Acknowledgements

DroidSource is based on [wtarit/scrcpy-obs](https://github.com/wtarit/scrcpy-obs).

DroidSource also relies on [scrcpy](https://github.com/Genymobile/scrcpy) and the companion [wtarit/scrcpy](https://github.com/wtarit/scrcpy) fork used to provide the raw-video transport and platform binaries.