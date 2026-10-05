# DroidSource

Android screen and camera source for OBS Studio, powered by [scrcpy](https://github.com/Genymobile/scrcpy). Each source captures a connected Android device without requiring scrcpy desktop window capture. Allow you to stream/record your Android device at native source resolution.

<img width="500" alt="scrcpy Source settings" src="https://github.com/user-attachments/assets/37bec7d2-c4f6-47ee-ae51-52c76e143af1" />

> **Status:** beta  
> Currently only support Windows with Linux planned, tested with OBS 32

## Requirements

- **Android device** with [USB debugging](https://developer.android.com/studio/debug/dev-options) enabled

## Install

1. Download the latest **Windows installer** from [Releases](https://github.com/DLC86/DroidSource/releases).
2. Run the installer and restart OBS.

The source appears as **DroidSource** in the Add Source menu.

> Note: You can also use the zipped version of the plugin directly by unzipping it and place the plugin in obs plugins folder.

## Usage

1. Connect your Android device.
2. In OBS, click **+** → **DroidSource** → create the source.
3. Approve the USB debugging prompt on the phone.

You can add multiple sources for multiple devices (one source per device).

## Source settings

| Setting             | Description                                                   |
| ------------------- | ------------------------------------------------------------- |
| **Device**          | Android device from the ADB device list.                      |
| **Refresh Devices** | Update devices list.                                          |
| **Video source**    | **Display** (screen mirror) or **Camera** (device camera).    |
| **Camera ID**       | Camera index when Video source is Camera (0–9).               |
| **Max size**        | Longest-edge cap in pixels; `0` = native resolution.          |
| **Bitrate (Kbps)**  | Encoder bitrate (default 8000). Lower if you see lag.         |
| **Video codec**     | H.264 (default), H.265, or AV1. H.264 is the most compatible. |

## Troubleshooting

**Device not in the list**

- Unlock the phone and accept the USB debugging authorization dialog.
- Click **Refresh Devices**.

## Building from source

For developers and contributors: [CONTRIBUTING.md](CONTRIBUTING.md).
