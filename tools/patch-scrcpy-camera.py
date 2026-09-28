#!/usr/bin/env python3
from pathlib import Path

ROOT = Path("scrcpy")
CAMERA_CONTROL_OPTION = "__scrcpy_obs_camera_control_port"


def patch(path, replacements):
    p = ROOT / path
    s = p.read_text(encoding="utf-8")
    for old, new in replacements:
        if old not in s:
            raise SystemExit(f"Patch pattern not found in {path}: {old[:160]!r}")
        s = s.replace(old, new, 1)
    p.write_text(s, encoding="utf-8")


patch("server/src/main/java/com/genymobile/scrcpy/Options.java", [
    (
        """public class Options {

    private Ln.Level logLevel = Ln.Level.DEBUG;
""",
        """public class Options {

    private static final String CAMERA_CONTROL_OPTION = "__scrcpy_obs_camera_control_port";

    private Ln.Level logLevel = Ln.Level.DEBUG;
""",
    ),
    (
        """    private boolean cameraTorch;
    private boolean showTouches;
""",
        """    private boolean cameraTorch;
    private int cameraControlPort;
    private boolean showTouches;
""",
    ),
    (
        """    public boolean getCameraTorch() {
        return cameraTorch;
    }

    public boolean getShowTouches() {
""",
        """    public boolean getCameraTorch() {
        return cameraTorch;
    }

    public int getCameraControlPort() {
        return cameraControlPort;
    }

    public boolean getShowTouches() {
""",
    ),
    (
        """                case "video_codec_options":
                    options.videoCodecOptions = CodecOption.parse(value);
                    break;
""",
        """                case "video_codec_options": {
                    List<CodecOption> codecOptions = CodecOption.parse(value);
                    if (codecOptions != null) {
                        for (int j = 0; j < codecOptions.size();) {
                            CodecOption option = codecOptions.get(j);
                            String optionKey = option.getKey();
                            Object valueObj = option.getValue();
                            if (CAMERA_CONTROL_OPTION.equals(optionKey)
                                    && valueObj instanceof Integer) {
                                int port = (Integer) valueObj;
                                if (port < 1 || port > 65535) {
                                    throw new IllegalArgumentException("Invalid camera control port: " + port);
                                }
                                options.cameraControlPort = port;
                                codecOptions.remove(j);
                            } else {
                                ++j;
                            }
                        }
                    }
                    options.videoCodecOptions = codecOptions;
                    break;
                }
""",
    ),
])


patch("server/src/main/java/com/genymobile/scrcpy/util/LogUtils.java", [
    (
        """                    if (Build.VERSION.SDK_INT >= AndroidVersions.API_30_ANDROID_11) {
                        try {
                            Range<Float> zoomRange = characteristics.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
                            if (zoomRange != null) {
                                String zoom = getFormattedZoomRange(zoomRange);
                                builder.append(", zoom-range=").append(zoom);
                            }
                        } catch (Exception e) {
                            Ln.w("Could not get available zoom ranges for camera " + id, e);
                        }
                    }

                    builder.append(')');
""",
        """                    if (Build.VERSION.SDK_INT >= AndroidVersions.API_30_ANDROID_11) {
                        try {
                            Range<Float> zoomRange = characteristics.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
                            if (zoomRange != null) {
                                String zoom = getFormattedZoomRange(zoomRange);
                                builder.append(", zoom-range=").append(zoom);
                            }
                        } catch (Exception e) {
                            Ln.w("Could not get available zoom ranges for camera " + id, e);
                        }
                    }

                    Range<Integer> isoRange = characteristics.get(
                            CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE);
                    if (isoRange != null) {
                        builder.append(", iso-range=[")
                                .append(isoRange.getLower()).append(", ")
                                .append(isoRange.getUpper()).append(']');
                    }

                    Range<Long> exposureRange = characteristics.get(
                            CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE);
                    if (exposureRange != null) {
                        builder.append(", exposure-time-range-ns=[")
                                .append(exposureRange.getLower()).append(", ")
                                .append(exposureRange.getUpper()).append(']');
                    }

                    Range<Integer> postRawBoostRange = characteristics.get(
                            CameraCharacteristics.CONTROL_POST_RAW_SENSITIVITY_BOOST_RANGE);
                    if (postRawBoostRange != null) {
                        builder.append(", post-raw-sensitivity-boost-range=[")
                                .append(postRawBoostRange.getLower()).append(", ")
                                .append(postRawBoostRange.getUpper()).append(']');
                    }

                    int[] capabilitiesForWb = characteristics.get(
                            CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
                    int[] correctionModesForWb = characteristics.get(
                            CameraCharacteristics.COLOR_CORRECTION_AVAILABLE_MODES);
                    int[] awbModesForWb = characteristics.get(
                            CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES);
                    boolean manualWb = contains(capabilitiesForWb,
                            CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING)
                            && contains(awbModesForWb, CaptureRequest.CONTROL_AWB_MODE_OFF)
                            && contains(correctionModesForWb,
                                    CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX);
                    if (manualWb) {
                        builder.append(", wb-manual=true");
                    }

                    Float focusMax = characteristics.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE);
                    if (focusMax != null && focusMax > 0) {
                        builder.append(", focus-range=[0, ")
                                .append(String.format(java.util.Locale.ROOT, "%.3f", focusMax))
                                .append(']');
                    }

                    if (Build.VERSION.SDK_INT >= 36) {
                        Range<Integer> cctRange =
                                characteristics.get(CameraCharacteristics.COLOR_CORRECTION_COLOR_TEMPERATURE_RANGE);
                        int[] correctionModes =
                                characteristics.get(CameraCharacteristics.COLOR_CORRECTION_AVAILABLE_MODES);
                        if (cctRange != null && correctionModes != null) {
                            boolean cct = false;
                            for (int mode : correctionModes) {
                                if (mode == android.hardware.camera2.CaptureRequest.COLOR_CORRECTION_MODE_CCT) {
                                    cct = true;
                                    break;
                                }
                            }
                            if (cct) {
                                builder.append(", wb-kelvin-range=[")
                                        .append(cctRange.getLower()).append(", ")
                                        .append(cctRange.getUpper()).append(']');
                            }
                        }
                    }
                    boolean wbPreset = false;
                    int[] awbModes = characteristics.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES);
                    if (awbModes != null) {
                        final int[] presetModes = {
                                android.hardware.camera2.CaptureRequest.CONTROL_AWB_MODE_INCANDESCENT,
                                android.hardware.camera2.CaptureRequest.CONTROL_AWB_MODE_FLUORESCENT,
                                android.hardware.camera2.CaptureRequest.CONTROL_AWB_MODE_WARM_FLUORESCENT,
                                android.hardware.camera2.CaptureRequest.CONTROL_AWB_MODE_DAYLIGHT,
                                android.hardware.camera2.CaptureRequest.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT,
                                android.hardware.camera2.CaptureRequest.CONTROL_AWB_MODE_TWILIGHT,
                                android.hardware.camera2.CaptureRequest.CONTROL_AWB_MODE_SHADE,
                        };
                        for (int available : awbModes) {
                            for (int preset : presetModes) {
                                if (available == preset) {
                                    wbPreset = true;
                                    break;
                                }
                            }
                            if (wbPreset) {
                                break;
                            }
                        }
                    }
                    if (wbPreset) {
                        builder.append(", wb-presets=true");
                    }


                    builder.append(')');
""",
    ),
])


patch("server/src/main/java/com/genymobile/scrcpy/video/CameraCapture.java", [
    (
        """import android.hardware.camera2.params.ColorSpaceTransform;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.RggbChannelVector;
import android.hardware.camera2.params.SessionConfiguration;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.util.Rational;
""",
        """import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.hardware.camera2.params.StreamConfigurationMap;
""",
    ),
    (
        """    private final boolean initialTorch;
    private float zoom;
""",
        """    private final boolean initialTorch;
    private final int cameraControlPort;
    private float zoom;
    private boolean torchEnabled;
    private int manualIso;
    private int manualShutterUs;
    private float manualFocusDistance;
    private int whiteBalanceKelvin;

    private CameraControlServer cameraControlServer;
""",
    ),
    (
        """    private Range<Float> zoomRange;

    private AffineMatrix transform;
""",
        """    private Range<Float> zoomRange;
    private CameraCharacteristics cameraCharacteristics;

    private AffineMatrix transform;
""",
    ),
    (
        """        this.initialTorch = options.getCameraTorch();
        this.zoom = options.getCameraZoom();
""",
        """        this.initialTorch = options.getCameraTorch();
        this.cameraControlPort = options.getCameraControlPort();
        this.zoom = options.getCameraZoom();
        this.torchEnabled = initialTorch;
""",
    ),
    (
        """            Ln.i("Using camera '" + cameraId + "'");
            cameraDevice = openCamera(cameraId);
""",
        """            Ln.i("Using camera '" + cameraId + "'");

            if (cameraControlPort > 0) {
                try {
                    cameraControlServer = new CameraControlServer(this, cameraControlPort);
                    cameraControlServer.start();
                } catch (IOException e) {
                    Ln.w("Could not start camera control server: " + e.getMessage());
                    cameraControlServer = null;
                }
            }

            cameraDevice = openCamera(cameraId);
""",
    ),
    (
        """                    CameraCharacteristics characteristics = cameraManager.getCameraCharacteristics(cameraId);
                    zoomRange = characteristics.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
""",
        """                    CameraCharacteristics characteristics = cameraManager.getCameraCharacteristics(cameraId);
                    zoomRange = characteristics.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
                    cameraCharacteristics = characteristics;
""",
    ),
    (
        """                    if (initialTorch) {
                        Ln.i("Turn camera torch on");
                        requestBuilder.set(CaptureRequest.FLASH_MODE, CaptureRequest.FLASH_MODE_TORCH);
                    }
                    if (zoom != 1) {
                        zoom = clampZoom(zoom);
                        Ln.i("Set camera zoom: " + zoom);
                        requestBuilder.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom);
                    }

                    CaptureRequest request = requestBuilder.build();
""",
        """                    try {
                        applyCurrentCameraSettings();
                    } catch (RuntimeException e) {
                        Ln.w("Could not apply initial camera settings: " + e.getMessage());
                    }

                    CaptureRequest request = requestBuilder.build();
""",
    ),
    (
        """    @Override
    public void release() {
        if (cameraDevice != null) {
""",
        """    @Override
    public void release() {
        if (cameraControlServer != null) {
            cameraControlServer.stop();
            cameraControlServer = null;
        }
        if (cameraDevice != null) {
""",
    ),
])


p = ROOT / "server/src/main/java/com/genymobile/scrcpy/video/CameraCapture.java"
s = p.read_text(encoding="utf-8")
marker = """    @TargetApi(AndroidVersions.API_30_ANDROID_11)
    private void zoom(boolean in) {
"""
methods = r'''    public void setCameraSettings(float zoomValue, boolean torch, int iso, int shutterUs,
                                  float focusDistance, int wbKelvin) {
        cameraHandler.post(() -> {
            assertCameraThread();
            zoom = zoomValue;
            torchEnabled = torch;
            manualIso = Math.max(0, iso);
            manualShutterUs = Math.max(0, shutterUs);
            manualFocusDistance = Math.max(0, focusDistance);
            whiteBalanceKelvin = Math.max(0, wbKelvin);

            if (currentSession != null && requestBuilder != null) {
                try {
                    applyCurrentCameraSettings();
                    setRepeatingRequest(currentSession, requestBuilder.build());
                } catch (CameraAccessException | IllegalArgumentException | IllegalStateException e) {
                    Ln.e("Camera error while applying settings: " + e.getMessage());
                }
            }
        });
    }

    private void applyCurrentCameraSettings() {
        assertCameraThread();
        if (cameraCharacteristics == null || requestBuilder == null) {
            return;
        }

        zoom = clampZoom(zoom);
        if (android.os.Build.VERSION.SDK_INT >= 30 && zoomRange != null) {
            requestBuilder.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom);
        } else {
            Rect activeArray = cameraCharacteristics.get(
                    CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
            if (activeArray != null) {
                float safeZoom = Math.max(1f, zoom);
                int cropWidth = Math.max(1, Math.round(activeArray.width() / safeZoom));
                int cropHeight = Math.max(1, Math.round(activeArray.height() / safeZoom));
                int left = activeArray.left + (activeArray.width() - cropWidth) / 2;
                int top = activeArray.top + (activeArray.height() - cropHeight) / 2;
                requestBuilder.set(CaptureRequest.SCALER_CROP_REGION,
                        new Rect(left, top, left + cropWidth, top + cropHeight));
            }
        }

        try {
            applyExposure();
        } catch (RuntimeException e) {
            Ln.w("Could not apply camera exposure: " + e.getMessage());
        }
        try {
            applyFocus();
        } catch (RuntimeException e) {
            Ln.w("Could not apply camera focus: " + e.getMessage());
        }
        try {
            applyWhiteBalance();
        } catch (RuntimeException e) {
            Ln.w("Could not apply camera white balance: " + e.getMessage());
            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                    CaptureRequest.CONTROL_AWB_MODE_AUTO);
            if (android.os.Build.VERSION.SDK_INT >= 36) {
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TEMPERATURE, null);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TINT, null);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE,
                        CaptureRequest.COLOR_CORRECTION_MODE_FAST);
            }
        }

        Boolean flashAvailable =
                cameraCharacteristics.get(CameraCharacteristics.FLASH_INFO_AVAILABLE);
        if (Boolean.TRUE.equals(flashAvailable)) {
            try {
                ServiceManager.getCameraManager().setTorchMode(cameraId, torchEnabled);
            } catch (CameraAccessException | IllegalArgumentException e) {
                Ln.w("CameraManager torch control failed: " + e.getMessage());
            }
        }
        requestBuilder.set(CaptureRequest.FLASH_MODE,
                torchEnabled && Boolean.TRUE.equals(flashAvailable)
                        ? CaptureRequest.FLASH_MODE_TORCH
                        : CaptureRequest.FLASH_MODE_OFF);
    }

    private void clearManualExposureKeys() {
        requestBuilder.set(CaptureRequest.SENSOR_SENSITIVITY, null);
        requestBuilder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, null);
        requestBuilder.set(CaptureRequest.SENSOR_FRAME_DURATION, null);
    }

    private void applyExposure() {
        assertCameraThread();

        boolean hasIso = manualIso > 0;
        boolean hasShutter = manualShutterUs > 0;

        Range<Integer> isoRange =
                cameraCharacteristics.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE);
        Range<Long> exposureRange =
                cameraCharacteristics.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE);

        if (!hasIso && !hasShutter) {
            clearManualExposureKeys();
            requestBuilder.set(CaptureRequest.CONTROL_AE_MODE,
                    CaptureRequest.CONTROL_AE_MODE_ON);
            if (android.os.Build.VERSION.SDK_INT >= 36) {
                requestBuilder.set(CaptureRequest.CONTROL_AE_PRIORITY_MODE,
                        CaptureRequest.CONTROL_AE_PRIORITY_MODE_OFF);
            }
            return;
        }

        if (isoRange == null || exposureRange == null) {
            Ln.w("Camera does not expose manual ISO/exposure ranges");
            clearManualExposureKeys();
            requestBuilder.set(CaptureRequest.CONTROL_AE_MODE,
                    CaptureRequest.CONTROL_AE_MODE_ON);
            return;
        }

        int[] capabilities =
                cameraCharacteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
        boolean manualSensor = contains(capabilities,
                CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR);

        if (hasIso && hasShutter) {
            if (!manualSensor) {
                Ln.w("Camera does not expose MANUAL_SENSOR; using automatic exposure");
                clearManualExposureKeys();
                requestBuilder.set(CaptureRequest.CONTROL_AE_MODE,
                        CaptureRequest.CONTROL_AE_MODE_ON);
                return;
            }

            int iso = isoRange.clamp(manualIso);
            long exposureNs = exposureRange.clamp(manualShutterUs * 1000L);
            requestBuilder.set(CaptureRequest.CONTROL_AE_MODE,
                    CaptureRequest.CONTROL_AE_MODE_OFF);
            if (android.os.Build.VERSION.SDK_INT >= 36) {
                requestBuilder.set(CaptureRequest.CONTROL_AE_PRIORITY_MODE,
                        CaptureRequest.CONTROL_AE_PRIORITY_MODE_OFF);
            }
            requestBuilder.set(CaptureRequest.SENSOR_SENSITIVITY, iso);
            requestBuilder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, exposureNs);
            long frameDurationNs = fps > 0 ? 1_000_000_000L / fps : exposureNs;
            requestBuilder.set(CaptureRequest.SENSOR_FRAME_DURATION,
                    Math.max(exposureNs, frameDurationNs));
            return;
        }

        if (android.os.Build.VERSION.SDK_INT < 36) {
            Ln.w("Independent ISO/shutter control requires Android 16");
            clearManualExposureKeys();
            requestBuilder.set(CaptureRequest.CONTROL_AE_MODE,
                    CaptureRequest.CONTROL_AE_MODE_ON);
            return;
        }

        int[] priorityModes = cameraCharacteristics.get(
                CameraCharacteristics.CONTROL_AE_AVAILABLE_PRIORITY_MODES);

        if (hasIso) {
            if (!contains(priorityModes,
                    CaptureRequest.CONTROL_AE_PRIORITY_MODE_SENSOR_SENSITIVITY_PRIORITY)) {
                Ln.w("Sensor-sensitivity AE priority mode is unavailable");
                clearManualExposureKeys();
                requestBuilder.set(CaptureRequest.CONTROL_AE_MODE,
                        CaptureRequest.CONTROL_AE_MODE_ON);
                return;
            }

            int iso = isoRange.clamp(manualIso);
            requestBuilder.set(CaptureRequest.CONTROL_AE_MODE,
                    CaptureRequest.CONTROL_AE_MODE_ON);
            requestBuilder.set(CaptureRequest.CONTROL_AE_PRIORITY_MODE,
                    CaptureRequest.CONTROL_AE_PRIORITY_MODE_SENSOR_SENSITIVITY_PRIORITY);
            requestBuilder.set(CaptureRequest.SENSOR_SENSITIVITY, iso);
            requestBuilder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, null);
            requestBuilder.set(CaptureRequest.SENSOR_FRAME_DURATION, null);
        } else {
            if (!contains(priorityModes,
                    CaptureRequest.CONTROL_AE_PRIORITY_MODE_SENSOR_EXPOSURE_TIME_PRIORITY)) {
                Ln.w("Sensor-exposure-time AE priority mode is unavailable");
                clearManualExposureKeys();
                requestBuilder.set(CaptureRequest.CONTROL_AE_MODE,
                        CaptureRequest.CONTROL_AE_MODE_ON);
                return;
            }

            long exposureNs = exposureRange.clamp(manualShutterUs * 1000L);
            requestBuilder.set(CaptureRequest.CONTROL_AE_MODE,
                    CaptureRequest.CONTROL_AE_MODE_ON);
            requestBuilder.set(CaptureRequest.CONTROL_AE_PRIORITY_MODE,
                    CaptureRequest.CONTROL_AE_PRIORITY_MODE_SENSOR_EXPOSURE_TIME_PRIORITY);
            requestBuilder.set(CaptureRequest.SENSOR_SENSITIVITY, null);
            requestBuilder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, exposureNs);
            requestBuilder.set(CaptureRequest.SENSOR_FRAME_DURATION, null);
        }
    }

    private void applyFocus() {
        assertCameraThread();

        int[] modes =
                cameraCharacteristics.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES);
        Float minimumFocusDistance =
                cameraCharacteristics.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE);

        boolean manualSupported = minimumFocusDistance != null
                && minimumFocusDistance > 0
                && contains(modes, CaptureRequest.CONTROL_AF_MODE_OFF);

        if (manualFocusDistance > 0 && manualSupported) {
            float distance = Math.min(manualFocusDistance, minimumFocusDistance);
            requestBuilder.set(CaptureRequest.CONTROL_AF_MODE,
                    CaptureRequest.CONTROL_AF_MODE_OFF);
            requestBuilder.set(CaptureRequest.LENS_FOCUS_DISTANCE, distance);
            return;
        }

        requestBuilder.set(CaptureRequest.LENS_FOCUS_DISTANCE, null);
        if (contains(modes, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)) {
            requestBuilder.set(CaptureRequest.CONTROL_AF_MODE,
                    CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
        } else if (contains(modes, CaptureRequest.CONTROL_AF_MODE_AUTO)) {
            requestBuilder.set(CaptureRequest.CONTROL_AF_MODE,
                    CaptureRequest.CONTROL_AF_MODE_AUTO);
        }
    }

    private void applyWhiteBalance() {
        assertCameraThread();

        if (whiteBalanceKelvin <= 0) {
            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                    CaptureRequest.CONTROL_AWB_MODE_AUTO);
            if (android.os.Build.VERSION.SDK_INT >= 36) {
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TEMPERATURE, null);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TINT, null);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE,
                        CaptureRequest.COLOR_CORRECTION_MODE_FAST);
            }
            return;
        }

        if (android.os.Build.VERSION.SDK_INT >= 36) {
            Range<Integer> cctRange = cameraCharacteristics.get(
                    CameraCharacteristics.COLOR_CORRECTION_COLOR_TEMPERATURE_RANGE);
            int[] correctionModes = cameraCharacteristics.get(
                    CameraCharacteristics.COLOR_CORRECTION_AVAILABLE_MODES);
            boolean cctSupported = cctRange != null
                    && contains(correctionModes, CaptureRequest.COLOR_CORRECTION_MODE_CCT);

            if (cctSupported) {
                requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                        CaptureRequest.CONTROL_AWB_MODE_OFF);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE,
                        CaptureRequest.COLOR_CORRECTION_MODE_CCT);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TEMPERATURE,
                        cctRange.clamp(whiteBalanceKelvin));
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TINT, 0);
                return;
            }
        }

        int[] awbModes =
                cameraCharacteristics.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES);
        requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TEMPERATURE, null);
        requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TINT, null);
        if (android.os.Build.VERSION.SDK_INT >= 36) {
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE,
                    CaptureRequest.COLOR_CORRECTION_MODE_FAST);
        }
        int wbMode = chooseAwbMode(whiteBalanceKelvin, awbModes);
        if (wbMode != CaptureRequest.CONTROL_AWB_MODE_AUTO) {
            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE, wbMode);
            Ln.i("Camera white balance preset: " + whiteBalanceKelvin + " K -> AWB mode " + wbMode);
        } else {
            Ln.w("Camera does not expose a usable manual white balance control");
            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                    CaptureRequest.CONTROL_AWB_MODE_AUTO);
        }
    }

    private static int chooseAwbMode(int kelvin, int[] availableModes) {
        if (availableModes == null) {
            return CaptureRequest.CONTROL_AWB_MODE_AUTO;
        }
        int[][] candidates = {
                {3000, CaptureRequest.CONTROL_AWB_MODE_INCANDESCENT},
                {4000, CaptureRequest.CONTROL_AWB_MODE_FLUORESCENT},
                {3500, CaptureRequest.CONTROL_AWB_MODE_WARM_FLUORESCENT},
                {5500, CaptureRequest.CONTROL_AWB_MODE_DAYLIGHT},
                {6500, CaptureRequest.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT},
                {7000, CaptureRequest.CONTROL_AWB_MODE_TWILIGHT},
                {7500, CaptureRequest.CONTROL_AWB_MODE_SHADE},
        };
        int bestMode = CaptureRequest.CONTROL_AWB_MODE_AUTO;
        int bestDistance = Integer.MAX_VALUE;
        for (int[] candidate : candidates) {
            if (!contains(availableModes, candidate[1])) {
                continue;
            }
            int distance = Math.abs(kelvin - candidate[0]);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestMode = candidate[1];
            }
        }
        return bestMode;
    }

    private static boolean contains(int[] values, int value) {
        if (values == null) {
            return false;
        }
        for (int item : values) {
            if (item == value) {
                return true;
            }
        }
        return false;
    }

'''
if marker not in s:
    raise SystemExit("CameraCapture insertion marker not found")
s = s.replace(marker, methods + marker, 1)
p.write_text(s, encoding="utf-8")


p = ROOT / "server/src/main/java/com/genymobile/scrcpy/video/CameraControlServer.java"
p.write_text("""package com.genymobile.scrcpy.video;

import com.genymobile.scrcpy.util.Ln;

import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketException;

final class CameraControlServer {

    private static final int TYPE_SETTINGS = 6;
    private static final int SETTINGS_SIZE = 21;

    private final CameraCapture capture;
    private final int port;

    private volatile boolean stopped;
    private ServerSocket serverSocket;
    private Socket clientSocket;
    private Thread thread;

    CameraControlServer(CameraCapture capture, int port) {
        this.capture = capture;
        this.port = port;
    }

    void start() throws IOException {
        serverSocket = new ServerSocket(port, 1, InetAddress.getLoopbackAddress());
        serverSocket.setReuseAddress(true);
        thread = new Thread(this::run, "camera-control");
        thread.start();
        Ln.i("Camera control server listening on 127.0.0.1:" + port);
    }

    void stop() {
        stopped = true;
        closeSocket(clientSocket);
        closeSocket(serverSocket);
        clientSocket = null;
        serverSocket = null;
        if (thread != null && Thread.currentThread() != thread) {
            try {
                thread.join(1000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
        thread = null;
    }

    private void run() {
        while (!stopped) {
            Socket socket = null;
            try {
                socket = serverSocket.accept();
                if (stopped) {
                    closeSocket(socket);
                    break;
                }
                clientSocket = socket;
                socket.setTcpNoDelay(true);
                readMessages(socket);
            } catch (SocketException e) {
                if (!stopped) {
                    Ln.w("Camera control socket error: " + e.getMessage());
                }
            } catch (IOException e) {
                if (!stopped) {
                    Ln.w("Camera control connection error: " + e.getMessage());
                }
            } finally {
                closeSocket(socket);
                if (clientSocket == socket) {
                    clientSocket = null;
                }
            }
        }
    }

    private void readMessages(Socket socket) throws IOException {
        DataInputStream in = new DataInputStream(socket.getInputStream());
        while (!stopped) {
            int type;
            try {
                type = in.readUnsignedByte();
            } catch (EOFException e) {
                return;
            }

            int size = in.readUnsignedByte();
            if (type != TYPE_SETTINGS) {
                throw new IOException("Unknown camera control message type: " + type);
            }
            if (size != SETTINGS_SIZE) {
                throw new IOException("Invalid camera control payload size: " + size);
            }

            float zoom = in.readFloat();
            boolean torch = in.readUnsignedByte() != 0;
            int iso = in.readInt();
            int shutterUs = in.readInt();
            float focusDistance = in.readFloat();
            int wbKelvin = in.readInt();

            capture.setCameraSettings(zoom, torch, iso, shutterUs, focusDistance, wbKelvin);
        }
    }

    private static void closeSocket(ServerSocket socket) {
        if (socket != null) {
            try {
                socket.close();
            } catch (IOException e) {
                // ignore during shutdown
            }
        }
    }

    private static void closeSocket(Socket socket) {
        if (socket != null) {
            try {
                socket.close();
            } catch (IOException e) {
                // ignore during shutdown
            }
        }
    }
}
""", encoding="utf-8")

print("scrcpy camera patch applied")
