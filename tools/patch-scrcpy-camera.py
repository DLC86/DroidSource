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
("""public class Options {

    private Ln.Level logLevel = Ln.Level.DEBUG;
""",
"""public class Options {

    private static final String CAMERA_CONTROL_OPTION = "__scrcpy_obs_camera_control_port";

    private Ln.Level logLevel = Ln.Level.DEBUG;
"""),
("""    private boolean cameraTorch;
    private boolean showTouches;
""",
"""    private boolean cameraTorch;
    private int cameraControlPort;
    private boolean showTouches;
"""),
("""    public boolean getCameraTorch() {
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
"""),
("""                case "video_codec_options":
                    options.videoCodecOptions = CodecOption.parse(value);
                    break;
""",
"""                case "video_codec_options": {
                    List<CodecOption> codecOptions = CodecOption.parse(value);
                    if (codecOptions != null) {
                        for (int j = 0; j < codecOptions.size();) {
                            CodecOption option = codecOptions.get(j);
                            if (CAMERA_CONTROL_OPTION.equals(option.getKey())
                                    && option.getValue() instanceof Integer) {
                                int port = (Integer) option.getValue();
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
"""),
])

patch("server/src/main/java/com/genymobile/scrcpy/video/CameraCapture.java", [
("""import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.SessionConfiguration;
import android.hardware.camera2.params.StreamConfigurationMap;
""",
"""import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.RggbChannelVector;
import android.hardware.camera2.params.SessionConfiguration;
import android.hardware.camera2.params.StreamConfigurationMap;
"""),
("""    private final boolean initialTorch;
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
"""),
("""    private Range<Float> zoomRange;

    private AffineMatrix transform;
""",
"""    private Range<Float> zoomRange;
    private CameraCharacteristics cameraCharacteristics;

    private AffineMatrix transform;
"""),
("""        this.initialTorch = options.getCameraTorch();
        this.zoom = options.getCameraZoom();
""",
"""        this.initialTorch = options.getCameraTorch();
        this.cameraControlPort = options.getCameraControlPort();
        this.zoom = options.getCameraZoom();
        this.torchEnabled = initialTorch;
"""),
("""            Ln.i("Using camera '" + cameraId + "'");
            cameraDevice = openCamera(cameraId);
        } catch (CameraAccessException | InterruptedException e) {
""",
"""            Ln.i("Using camera '" + cameraId + "'");
            cameraDevice = openCamera(cameraId);

            if (cameraControlPort > 0) {
                try {
                    cameraControlServer = new CameraControlServer(this, cameraControlPort);
                    cameraControlServer.start();
                } catch (IOException e) {
                    Ln.w("Could not start camera control server: " + e.getMessage());
                    cameraControlServer = null;
                }
            }
        } catch (CameraAccessException | InterruptedException e) {
"""),
("""                    CameraCharacteristics characteristics = cameraManager.getCameraCharacteristics(cameraId);
                    zoomRange = characteristics.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
""",
"""                    CameraCharacteristics characteristics = cameraManager.getCameraCharacteristics(cameraId);
                    zoomRange = characteristics.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
                    cameraCharacteristics = characteristics;
"""),
("""                    if (initialTorch) {
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
"""),
("""    @Override
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
"""),
("""    public void setTorchEnabled(boolean enabled) {
        cameraHandler.post(() -> {
            assertCameraThread();
            if (currentSession != null && requestBuilder != null) {
                try {
                    Ln.i("Turn camera torch " + (enabled ? "on" : "off"));
                    requestBuilder.set(CaptureRequest.FLASH_MODE, enabled ? CaptureRequest.FLASH_MODE_TORCH : CaptureRequest.FLASH_MODE_OFF);
                    CaptureRequest request = requestBuilder.build();
                    setRepeatingRequest(currentSession, request);
                } catch (CameraAccessException e) {
                    Ln.e("Camera error", e);
                }
            }
        });
    }
""",
"""    public void setTorchEnabled(boolean enabled) {
        cameraHandler.post(() -> {
            assertCameraThread();
            torchEnabled = enabled;
            if (currentSession != null && requestBuilder != null) {
                try {
                    requestBuilder.set(CaptureRequest.FLASH_MODE,
                            enabled ? CaptureRequest.FLASH_MODE_TORCH : CaptureRequest.FLASH_MODE_OFF);
                    setRepeatingRequest(currentSession, requestBuilder.build());
                } catch (CameraAccessException e) {
                    Ln.e("Camera error while setting torch: " + e.getMessage());
                }
            }
        });
    }
"""),
])

p = ROOT / "server/src/main/java/com/genymobile/scrcpy/video/CameraCapture.java"
s = p.read_text(encoding="utf-8")
marker = """    @TargetApi(AndroidVersions.API_30_ANDROID_11)
    private void zoom(boolean in) {
"""
if marker not in s:
    raise SystemExit("zoom marker not found")
methods = r'''    public void setZoomRatio(float value) {
        cameraHandler.post(() -> {
            assertCameraThread();
            zoom = clampZoom(value);
            if (currentSession != null && requestBuilder != null) {
                try {
                    requestBuilder.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom);
                    setRepeatingRequest(currentSession, requestBuilder.build());
                } catch (CameraAccessException e) {
                    Ln.e("Camera error while setting zoom: " + e.getMessage());
                }
            }
        });
    }

    public void setManualExposure(int iso, int shutterUs) {
        cameraHandler.post(() -> {
            assertCameraThread();
            manualIso = Math.max(0, iso);
            manualShutterUs = Math.max(0, shutterUs);
            if (currentSession != null && requestBuilder != null) {
                try {
                    applyExposure();
                    setRepeatingRequest(currentSession, requestBuilder.build());
                } catch (CameraAccessException e) {
                    Ln.e("Camera error while setting exposure: " + e.getMessage());
                }
            }
        });
    }

    public void setFocusDistance(float distance) {
        cameraHandler.post(() -> {
            assertCameraThread();
            manualFocusDistance = Math.max(0, distance);
            if (currentSession != null && requestBuilder != null) {
                try {
                    applyFocus();
                    setRepeatingRequest(currentSession, requestBuilder.build());
                } catch (CameraAccessException e) {
                    Ln.e("Camera error while setting focus: " + e.getMessage());
                }
            }
        });
    }

    public void setWhiteBalanceKelvin(int kelvin) {
        cameraHandler.post(() -> {
            assertCameraThread();
            whiteBalanceKelvin = Math.max(0, kelvin);
            if (currentSession != null && requestBuilder != null) {
                try {
                    applyWhiteBalance();
                    setRepeatingRequest(currentSession, requestBuilder.build());
                } catch (CameraAccessException e) {
                    Ln.e("Camera error while setting white balance: " + e.getMessage());
                }
            }
        });
    }

    private void applyCurrentCameraSettings() throws CameraAccessException {
        assertCameraThread();

        requestBuilder.set(CaptureRequest.FLASH_MODE,
                torchEnabled ? CaptureRequest.FLASH_MODE_TORCH : CaptureRequest.FLASH_MODE_OFF);

        if (zoom != 1) {
            zoom = clampZoom(zoom);
            requestBuilder.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom);
        }

        applyExposure();
        applyFocus();
        applyWhiteBalance();
    }

    private void applyExposure() {
        assertCameraThread();

        if (manualIso <= 0 || manualShutterUs <= 0 || highSpeed || cameraCharacteristics == null) {
            requestBuilder.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
            return;
        }

        int[] capabilities =
                cameraCharacteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
        if (!contains(capabilities,
                CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)) {
            Ln.w("Manual exposure is not supported by this camera");
            requestBuilder.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
            return;
        }

        Range<Integer> isoRange =
                cameraCharacteristics.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE);
        Range<Long> exposureRange =
                cameraCharacteristics.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE);
        if (isoRange == null || exposureRange == null) {
            Ln.w("Manual exposure ranges are unavailable");
            requestBuilder.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
            return;
        }

        int iso = isoRange.clamp(manualIso);
        long exposureNs = exposureRange.clamp(manualShutterUs * 1000L);
        long frameDurationNs = fps > 0 ? 1_000_000_000L / fps : exposureNs;

        requestBuilder.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF);
        requestBuilder.set(CaptureRequest.SENSOR_SENSITIVITY, iso);
        requestBuilder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, exposureNs);
        requestBuilder.set(CaptureRequest.SENSOR_FRAME_DURATION,
                Math.max(exposureNs, frameDurationNs));
    }

    private void applyFocus() {
        assertCameraThread();
        if (cameraCharacteristics == null) {
            return;
        }

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
        } else if (contains(modes, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)) {
            requestBuilder.set(CaptureRequest.CONTROL_AF_MODE,
                    CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
        } else if (contains(modes, CaptureRequest.CONTROL_AF_MODE_AUTO)) {
            requestBuilder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO);
        }
    }

    private void applyWhiteBalance() {
        assertCameraThread();
        if (cameraCharacteristics == null) {
            return;
        }

        int[] modes =
                cameraCharacteristics.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES);
        int[] capabilities =
                cameraCharacteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);

        boolean manualSupported = contains(modes, CaptureRequest.CONTROL_AWB_MODE_OFF)
                && contains(capabilities,
                        CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING);

        if (whiteBalanceKelvin <= 0) {
            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                    CaptureRequest.CONTROL_AWB_MODE_AUTO);
        } else if (manualSupported) {
            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_OFF);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE,
                    CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_GAINS,
                    kelvinToGains(whiteBalanceKelvin));
        } else {
            Ln.w("Manual Kelvin white balance is not supported by this camera");
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
s=s.replace(marker,methods+marker,1)
p.write_text(s, encoding="utf-8")

p = ROOT / "server/src/main/java/com/genymobile/scrcpy/video/CameraControlServer.java"
p.write_text("package com.genymobile.scrcpy.video;\n\nimport com.genymobile.scrcpy.util.Ln;\n\nimport java.io.DataInputStream;\nimport java.io.EOFException;\nimport java.io.IOException;\nimport java.net.InetAddress;\nimport java.net.ServerSocket;\nimport java.net.Socket;\nimport java.net.SocketException;\n\nfinal class CameraControlServer {\n\n    private static final int TYPE_ZOOM = 1;\n    private static final int TYPE_TORCH = 2;\n    private static final int TYPE_EXPOSURE = 3;\n    private static final int TYPE_FOCUS = 4;\n    private static final int TYPE_WHITE_BALANCE = 5;\n\n    private final CameraCapture capture;\n    private final int port;\n\n    private volatile boolean stopped;\n    private ServerSocket serverSocket;\n    private Socket clientSocket;\n    private Thread thread;\n\n    CameraControlServer(CameraCapture capture, int port) {\n        this.capture = capture;\n        this.port = port;\n    }\n\n    void start() throws IOException {\n        serverSocket = new ServerSocket(port, 1, InetAddress.getLoopbackAddress());\n        serverSocket.setReuseAddress(true);\n        thread = new Thread(this::run, \"camera-control\");\n        thread.start();\n        Ln.i(\"Camera control server listening on 127.0.0.1:\" + port);\n    }\n\n    void stop() {\n        stopped = true;\n        closeSocket(clientSocket);\n        closeSocket(serverSocket);\n        clientSocket = null;\n        serverSocket = null;\n        if (thread != null && Thread.currentThread() != thread) {\n            try {\n                thread.join(1000);\n            } catch (InterruptedException e) {\n                Thread.currentThread().interrupt();\n            }\n        }\n        thread = null;\n    }\n\n    private void run() {\n        while (!stopped) {\n            Socket socket = null;\n            try {\n                socket = serverSocket.accept();\n                if (stopped) {\n                    closeSocket(socket);\n                    break;\n                }\n                clientSocket = socket;\n                socket.setTcpNoDelay(true);\n                readMessages(socket);\n            } catch (SocketException e) {\n                if (!stopped) {\n                    Ln.w(\"Camera control socket error: \" + e.getMessage());\n                }\n            } catch (IOException e) {\n                if (!stopped) {\n                    Ln.w(\"Camera control connection error: \" + e.getMessage());\n                }\n            } finally {\n                closeSocket(socket);\n                if (clientSocket == socket) {\n                    clientSocket = null;\n                }\n            }\n        }\n    }\n\n    private void readMessages(Socket socket) throws IOException {\n        DataInputStream in = new DataInputStream(socket.getInputStream());\n        while (!stopped) {\n            int type;\n            try {\n                type = in.readUnsignedByte();\n            } catch (EOFException e) {\n                return;\n            }\n\n            int size = in.readUnsignedByte();\n            switch (type) {\n                case TYPE_ZOOM:\n                    requireSize(type, size, 4);\n                    capture.setZoomRatio(in.readFloat());\n                    break;\n                case TYPE_TORCH:\n                    requireSize(type, size, 1);\n                    capture.setTorchEnabled(in.readUnsignedByte() != 0);\n                    break;\n                case TYPE_EXPOSURE:\n                    requireSize(type, size, 8);\n                    capture.setManualExposure(in.readInt(), in.readInt());\n                    break;\n                case TYPE_FOCUS:\n                    requireSize(type, size, 4);\n                    capture.setFocusDistance(in.readFloat());\n                    break;\n                case TYPE_WHITE_BALANCE:\n                    requireSize(type, size, 4);\n                    capture.setWhiteBalanceKelvin(in.readInt());\n                    break;\n                default:\n                    throw new IOException(\"Unknown camera control message type: \" + type);\n            }\n        }\n    }\n\n    private static void requireSize(int type, int actual, int expected) throws IOException {\n        if (actual != expected) {\n            throw new IOException(\"Invalid camera control message \" + type + \" size: \" + actual);\n        }\n    }\n\n    private static void closeSocket(ServerSocket socket) {\n        if (socket != null) {\n            try {\n                socket.close();\n            } catch (IOException e) {\n                // ignore during shutdown\n            }\n        }\n    }\n\n    private static void closeSocket(Socket socket) {\n        if (socket != null) {\n            try {\n                socket.close();\n            } catch (IOException e) {\n                // ignore during shutdown\n            }\n        }\n    }\n}\n", encoding="utf-8")
