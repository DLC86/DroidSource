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
    private int cameraIso;
    private int cameraShutterUs;
    private float cameraFocusDistance;
    private int cameraWbKelvin;
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

    public int getCameraIso() {
        return cameraIso;
    }

    public int getCameraShutterUs() {
        return cameraShutterUs;
    }

    public float getCameraFocusDistance() {
        return cameraFocusDistance;
    }

    public int getCameraWbKelvin() {
        return cameraWbKelvin;
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
                            } else if ("__scrcpy_obs_camera_iso".equals(optionKey) && valueObj instanceof Integer) {
                                options.cameraIso = (Integer) valueObj;
                                codecOptions.remove(j);
                            } else if ("__scrcpy_obs_camera_shutter".equals(optionKey) && valueObj instanceof Integer) {
                                options.cameraShutterUs = (Integer) valueObj;
                                codecOptions.remove(j);
                            } else if ("__scrcpy_obs_camera_focus".equals(optionKey) && valueObj instanceof Float) {
                                options.cameraFocusDistance = (Float) valueObj;
                                codecOptions.remove(j);
                            } else if ("__scrcpy_obs_camera_wb".equals(optionKey) && valueObj instanceof Integer) {
                                options.cameraWbKelvin = (Integer) valueObj;
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

                    Integer sensorOrientation = characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION);
                    if (sensorOrientation != null) {
                        builder.append(", sensor-orientation=").append(sensorOrientation);
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
                    boolean manualWb = false;
                    int[] awbModes = characteristics.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES);
                    int[] capabilitiesForWb = characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
                    if (awbModes != null && capabilitiesForWb != null) {
                        boolean awbOff = false;
                        boolean manualPostProcessing = false;
                        for (int mode : awbModes) {
                            if (mode == android.hardware.camera2.CaptureRequest.CONTROL_AWB_MODE_OFF) {
                                awbOff = true;
                                break;
                            }
                        }
                        for (int capability : capabilitiesForWb) {
                            if (capability == CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING) {
                                manualPostProcessing = true;
                                break;
                            }
                        }
                        manualWb = awbOff && manualPostProcessing;
                        if (Build.VERSION.SDK_INT >= 36) {
                            int[] correctionModesForWb =
                                    characteristics.get(CameraCharacteristics.COLOR_CORRECTION_AVAILABLE_MODES);
                            boolean transformMatrix = false;
                            if (correctionModesForWb != null) {
                                for (int mode : correctionModesForWb) {
                                    if (mode == android.hardware.camera2.CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX) {
                                        transformMatrix = true;
                                        break;
                                    }
                                }
                            }
                            manualWb = manualWb && transformMatrix;
                        } else {
                            // FULL-level cameras guarantee TRANSFORM_MATRIX on older Android releases.
                        }
                    }
                    if (manualWb) {
                        builder.append(", wb-manual=true");
                    }


                    builder.append(')');
""",
    ),
])


patch("server/src/main/java/com/genymobile/scrcpy/video/CameraCapture.java", [
    (
        """import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.hardware.camera2.params.StreamConfigurationMap;
""",
        """import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.RggbChannelVector;
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
        this.manualIso = Math.max(0, options.getCameraIso());
        this.manualShutterUs = Math.max(0, options.getCameraShutterUs());
        this.manualFocusDistance = Math.max(0, options.getCameraFocusDistance());
        this.whiteBalanceKelvin = Math.max(0, options.getCameraWbKelvin());
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
        """                    applyCurrentCameraSettings();

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
        Rect activeArray = cameraCharacteristics.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE);
        if (activeArray != null) {
            float safeZoom = Math.max(1f, zoom);
            int cropWidth = Math.max(1, Math.round(activeArray.width() / safeZoom));
            int cropHeight = Math.max(1, Math.round(activeArray.height() / safeZoom));
            int left = activeArray.left + (activeArray.width() - cropWidth) / 2;
            int top = activeArray.top + (activeArray.height() - cropHeight) / 2;
            requestBuilder.set(CaptureRequest.SCALER_CROP_REGION,
                    new Rect(left, top, left + cropWidth, top + cropHeight));
        } else {
            requestBuilder.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom);
        }

        applyExposure();
        applyFocus();
        applyWhiteBalance();

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
            requestBuilder.set(CaptureRequest.SENSOR_FRAME_DURATION, exposureNs);
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
            int[] capabilities = cameraCharacteristics.get(
                    CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);

            boolean cctSupported = cctRange != null
                    && contains(correctionModes, CaptureRequest.COLOR_CORRECTION_MODE_CCT)
                    && contains(capabilities,
                            CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING);

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
        int[] capabilities =
                cameraCharacteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
        int[] correctionModes =
                cameraCharacteristics.get(CameraCharacteristics.COLOR_CORRECTION_AVAILABLE_MODES);

        boolean manualSupported =
                contains(awbModes, CaptureRequest.CONTROL_AWB_MODE_OFF)
                && contains(capabilities,
                        CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING)
                && contains(correctionModes,
                        CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX);

        if (manualSupported) {
            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                    CaptureRequest.CONTROL_AWB_MODE_OFF);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE,
                    CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_GAINS,
                    kelvinToGains(whiteBalanceKelvin));
        } else {
            Ln.w("Manual white balance is unsupported on this camera");
            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                    CaptureRequest.CONTROL_AWB_MODE_AUTO);
        }
    }

    private static RggbChannelVector kelvinToGains(int kelvin) {
        double temperature = Math.max(1000, Math.min(15000, kelvin)) / 100.0;
        double red;
        double green;
        double blue;

        if (temperature <= 66) {
            red = 255;
        } else {
            red = 329.698727446 * Math.pow(temperature - 60, -0.1332047592);
        }

        if (temperature <= 66) {
            green = 99.4708025861 * Math.log(Math.max(1, temperature)) - 161.1195681661;
        } else {
            green = 288.1221695283 * Math.pow(temperature - 60, -0.0755148492);
        }

        if (temperature >= 66) {
            blue = 255;
        } else if (temperature <= 19) {
            blue = 1;
        } else {
            blue = 138.5177312231 * Math.log(temperature - 10) - 305.0447927307;
        }

        red = Math.max(1, Math.min(255, red));
        green = Math.max(1, Math.min(255, green));
        blue = Math.max(1, Math.min(255, blue));

        float redGain = (float) (green / red);
        float blueGain = (float) (green / blue);
        float maxGain = Math.max(redGain, blueGain);

        if (maxGain > 8f) {
            float scale = 8f / maxGain;
            redGain *= scale;
            blueGain *= scale;
        }

        return new RggbChannelVector(redGain, 1f, 1f, blueGain);
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
