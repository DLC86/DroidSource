#!/usr/bin/env python3
from pathlib import Path

ROOT = Path("scrcpy")

def patch(path, replacements):
    p = ROOT / path
    s = p.read_text(encoding="utf-8")
    for old, new in replacements:
        if old not in s:
            raise SystemExit(f"Patch pattern not found in {path}: {old[:120]!r}")
        s = s.replace(old, new, 1)
    p.write_text(s, encoding="utf-8")

patch("server/src/main/java/com/genymobile/scrcpy/Options.java", [
("""    private boolean cameraTorch;
""",
"""    private boolean cameraTorch;
    private int cameraIso;
    private int cameraShutterUs;
    private float cameraFocusDistance;
    private String cameraAwbMode;
"""),
("""    public boolean getCameraTorch() {
        return cameraTorch;
    }
""",
"""    public boolean getCameraTorch() {
        return cameraTorch;
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

    public String getCameraAwbMode() {
        return cameraAwbMode;
    }
"""),
("""                case "camera_torch":
                    options.cameraTorch = Boolean.parseBoolean(value);
                    break;
""",
"""                case "camera_torch":
                    options.cameraTorch = Boolean.parseBoolean(value);
                    break;
                case "camera_iso":
                    options.cameraIso = Integer.parseInt(value);
                    break;
                case "camera_shutter_us":
                    options.cameraShutterUs = Integer.parseInt(value);
                    break;
                case "camera_focus":
                    options.cameraFocusDistance = Float.parseFloat(value);
                    break;
                case "camera_awb":
                    options.cameraAwbMode = value;
                    break;
""")
])

patch("server/src/main/java/com/genymobile/scrcpy/video/CameraCapture.java", [
("""                CameraManager cameraManager = ServiceManager.getCameraManager();
                try {
                    CameraCharacteristics characteristics = cameraManager.getCameraCharacteristics(cameraId);
                    zoomRange = characteristics.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
                } catch (CameraAccessException e) {
                    Ln.w("Could not get camera characteristics");
                }

                try {
                    requestBuilder = cameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
                    requestBuilder.addTarget(captureSurface);

                    if (fps > 0) {
                        requestBuilder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, new Range<>(fps, fps));
                    }
                    if (initialTorch) {
                        Ln.i("Turn camera torch on");
                        requestBuilder.set(CaptureRequest.FLASH_MODE, CaptureRequest.FLASH_MODE_TORCH);
                    }
                    if (zoom != 1) {
                        zoom = clampZoom(zoom);
                        Ln.i("Set camera zoom: " + zoom);
                        requestBuilder.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom);
                    }

                    CaptureRequest request = requestBuilder.build();
                    setRepeatingRequest(session, request);
                    currentSession = session;
                } catch (CameraAccessException e) {
                    Ln.e("Camera error", e);
                    disconnected.set(true);
                    getCaptureControl().reset(CaptureControl.RESET_REASON_TERMINATED);
                }
""",
"""                CameraManager cameraManager = ServiceManager.getCameraManager();
                CameraCharacteristics characteristics = null;
                try {
                    characteristics = cameraManager.getCameraCharacteristics(cameraId);
                    zoomRange = characteristics.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE);
                } catch (CameraAccessException e) {
                    Ln.w("Could not get camera characteristics");
                }

                try {
                    requestBuilder = cameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
                    requestBuilder.addTarget(captureSurface);

                    boolean manualExposure = cameraIso > 0 && cameraShutterUs > 0
                            && hasManualSensorSupport(characteristics);
                    if (fps > 0 && !manualExposure) {
                        requestBuilder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, new Range<>(fps, fps));
                    }

                    applyManualCameraControls(requestBuilder, characteristics);

                    if (initialTorch) {
                        Ln.i("Turn camera torch on");
                        requestBuilder.set(CaptureRequest.FLASH_MODE, CaptureRequest.FLASH_MODE_TORCH);
                    }
                    if (zoom != 1) {
                        zoom = clampZoom(zoom);
                        Ln.i("Set camera zoom: " + zoom);
                        requestBuilder.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom);
                    }

                    CaptureRequest request = requestBuilder.build();
                    setRepeatingRequest(session, request);
                    currentSession = session;
                } catch (CameraAccessException | IllegalArgumentException e) {
                    if (hasManualControlRequest()) {
                        Ln.w("Camera rejected the requested manual controls; retrying automatically", e);
                        try {
                            requestBuilder = cameraDevice.createCaptureRequest(CameraDevice.TEMPLATE_RECORD);
                            requestBuilder.addTarget(captureSurface);
                            if (fps > 0) {
                                requestBuilder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, new Range<>(fps, fps));
                            }
                            if (initialTorch) {
                                requestBuilder.set(CaptureRequest.FLASH_MODE, CaptureRequest.FLASH_MODE_TORCH);
                            }
                            if (zoom != 1) {
                                zoom = clampZoom(zoom);
                                requestBuilder.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom);
                            }
                            request = requestBuilder.build();
                            setRepeatingRequest(session, request);
                            currentSession = session;
                        } catch (CameraAccessException | IllegalArgumentException fallbackError) {
                            Ln.e("Camera fallback request failed", fallbackError);
                            disconnected.set(true);
                            getCaptureControl().reset(CaptureControl.RESET_REASON_TERMINATED);
                        }
                    } else {
                        Ln.e("Camera error", e);
                        disconnected.set(true);
                        getCaptureControl().reset(CaptureControl.RESET_REASON_TERMINATED);
                    }
                }
"""),
("""    private final boolean initialTorch;
    private float zoom;
""",
"""    private final boolean initialTorch;
    private final int cameraIso;
    private final int cameraShutterUs;
    private final float cameraFocusDistance;
    private final String cameraAwbMode;
    private float zoom;
"""),
("""        this.initialTorch = options.getCameraTorch();
        this.zoom = options.getCameraZoom();
""",
"""        this.initialTorch = options.getCameraTorch();
        this.cameraIso = options.getCameraIso();
        this.cameraShutterUs = options.getCameraShutterUs();
        this.cameraFocusDistance = options.getCameraFocusDistance();
        this.cameraAwbMode = options.getCameraAwbMode();
        this.zoom = options.getCameraZoom();
"""),
("""    private float clampZoom(float value) {
""",
"""    private boolean hasManualControlRequest() {
        return cameraIso > 0
                || cameraShutterUs > 0
                || cameraFocusDistance > 0
                || (cameraAwbMode != null && !cameraAwbMode.isEmpty() && !"auto".equals(cameraAwbMode));
    }

    private static boolean contains(int[] values, int value) {
        if (values == null) {
            return false;
        }
        for (int candidate : values) {
            if (candidate == value) {
                return true;
            }
        }
        return false;
    }

    private static boolean hasManualSensorSupport(CameraCharacteristics characteristics) {
        if (characteristics == null) {
            return false;
        }

        int[] capabilities = characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
        if (!contains(capabilities, CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)) {
            return false;
        }

        int[] aeModes = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_MODES);
        return contains(aeModes, CaptureRequest.CONTROL_AE_MODE_OFF);
    }

    private static boolean hasAwbMode(CameraCharacteristics characteristics, int mode) {
        return characteristics != null && contains(
                characteristics.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES), mode);
    }

    private static boolean hasManualFocusSupport(CameraCharacteristics characteristics) {
        if (characteristics == null) {
            return false;
        }

        Float minFocusDistance = characteristics.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE);
        if (minFocusDistance == null || minFocusDistance <= 0) {
            return false;
        }

        int[] afModes = characteristics.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES);
        return contains(afModes, CaptureRequest.CONTROL_AF_MODE_OFF);
    }

    private void applyManualCameraControls(CaptureRequest.Builder builder, CameraCharacteristics characteristics) {
        if (cameraIso > 0 || cameraShutterUs > 0) {
            if (cameraIso > 0 && cameraShutterUs > 0 && hasManualSensorSupport(characteristics)) {
                Range<Integer> isoRange = characteristics.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE);
                Range<Long> exposureRange = characteristics.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE);
                if (isoRange != null && exposureRange != null) {
                    int iso = isoRange.clamp(cameraIso);
                    long exposureNs = exposureRange.clamp(cameraShutterUs * 1000L);
                    if (fps > 0) {
                        long frameDurationNs = 1000000000L / fps;
                        exposureNs = Math.min(exposureNs, frameDurationNs);
                    }
                    builder.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF);
                    builder.set(CaptureRequest.SENSOR_SENSITIVITY, iso);
                    builder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, exposureNs);
                    Ln.i("Set manual exposure: ISO " + iso + ", " + cameraShutterUs + " us");
                } else {
                    Ln.w("Manual exposure ranges are unavailable; keeping automatic exposure");
                }
            } else {
                Ln.w("Manual exposure is unavailable or incomplete; keeping automatic exposure");
            }
        }

        if (cameraFocusDistance > 0) {
            if (hasManualFocusSupport(characteristics)) {
                Float minFocusDistance = characteristics.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE);
                float focusDistance = Math.min(cameraFocusDistance, minFocusDistance);
                builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF);
                builder.set(CaptureRequest.LENS_FOCUS_DISTANCE, focusDistance);
                Ln.i("Set manual focus distance: " + focusDistance + " diopters");
            } else {
                Ln.w("Manual focus is unavailable; keeping automatic focus");
            }
        }

        if (cameraAwbMode != null && !cameraAwbMode.isEmpty() && !"auto".equals(cameraAwbMode)) {
            Integer awbMode = getAwbMode(cameraAwbMode);
            if (awbMode != null && hasAwbMode(characteristics, awbMode)) {
                builder.set(CaptureRequest.CONTROL_AWB_MODE, awbMode);
                Ln.i("Set white balance: " + cameraAwbMode);
            } else {
                Ln.w("White balance mode is unavailable; keeping automatic white balance");
            }
        }
    }

    private static Integer getAwbMode(String mode) {
        switch (mode) {
            case "incandescent":
                return CaptureRequest.CONTROL_AWB_MODE_INCANDESCENT;
            case "fluorescent":
                return CaptureRequest.CONTROL_AWB_MODE_FLUORESCENT;
            case "daylight":
                return CaptureRequest.CONTROL_AWB_MODE_DAYLIGHT;
            case "cloudy":
                return CaptureRequest.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT;
            default:
                return null;
        }
    }

    private float clampZoom(float value) {
""")
])

patch("app/src/options.h", [
("""    const char *camera_zoom;
    uint16_t camera_fps;
""",
"""    const char *camera_zoom;
    const char *camera_awb;
    const char *camera_focus;
    const char *camera_iso;
    const char *camera_shutter_us;
    uint16_t camera_fps;
""")
])

patch("app/src/options.c", [
("""    .camera_zoom = NULL,
    .camera_fps = 0,
""",
"""    .camera_zoom = NULL,
    .camera_awb = NULL,
    .camera_focus = NULL,
    .camera_iso = NULL,
    .camera_shutter_us = NULL,
    .camera_fps = 0,
""")
])

patch("app/src/server.c", [
("""    if (params->camera_zoom) {
        VALIDATE_STRING(params->camera_zoom);
        ADD_PARAM("camera_zoom=%s", params->camera_zoom);
    }
""",
"""    if (params->camera_zoom) {
        VALIDATE_STRING(params->camera_zoom);
        ADD_PARAM("camera_zoom=%s", params->camera_zoom);
    }
    if (params->camera_iso) {
        VALIDATE_STRING(params->camera_iso);
        ADD_PARAM("camera_iso=%s", params->camera_iso);
    }
    if (params->camera_shutter_us) {
        VALIDATE_STRING(params->camera_shutter_us);
        ADD_PARAM("camera_shutter_us=%s", params->camera_shutter_us);
    }
    if (params->camera_focus) {
        VALIDATE_STRING(params->camera_focus);
        ADD_PARAM("camera_focus=%s", params->camera_focus);
    }
    if (params->camera_awb) {
        VALIDATE_STRING(params->camera_awb);
        ADD_PARAM("camera_awb=%s", params->camera_awb);
    }
""")
])

patch("app/src/cli.c", [
("""    OPT_CAMERA_TORCH,
    OPT_CAMERA_ZOOM,
""",
"""    OPT_CAMERA_TORCH,
    OPT_CAMERA_ZOOM,
    OPT_CAMERA_ISO,
    OPT_CAMERA_SHUTTER,
    OPT_CAMERA_FOCUS,
    OPT_CAMERA_AWB,
"""),
("""    {
        .longopt_id = OPT_CAMERA_ZOOM,
        .longopt = "camera-zoom",
        .argdesc = "zoom",
        .text = "Specify the camera zoom initial value.",
    },
""",
"""    {
        .longopt_id = OPT_CAMERA_ZOOM,
        .longopt = "camera-zoom",
        .argdesc = "zoom",
        .text = "Specify the camera zoom initial value.",
    },
    {
        .longopt_id = OPT_CAMERA_ISO,
        .longopt = "camera-iso",
        .argdesc = "value",
        .text = "Set manual camera ISO. Must be used together with --camera-shutter.",
    },
    {
        .longopt_id = OPT_CAMERA_SHUTTER,
        .longopt = "camera-shutter",
        .argdesc = "microseconds",
        .text = "Set manual camera shutter time in microseconds. Must be used together with --camera-iso.",
    },
    {
        .longopt_id = OPT_CAMERA_FOCUS,
        .longopt = "camera-focus",
        .argdesc = "diopters",
        .text = "Set manual camera focus distance in diopters. 0 disables manual focus.",
    },
    {
        .longopt_id = OPT_CAMERA_AWB,
        .longopt = "camera-awb",
        .argdesc = "mode",
        .text = "Set camera white balance: auto, incandescent, fluorescent, daylight or cloudy.",
    },
"""),
("""            case OPT_CAMERA_ZOOM:
                opts->camera_zoom = optarg;
                break;
""",
"""            case OPT_CAMERA_ZOOM:
                opts->camera_zoom = optarg;
                break;
            case OPT_CAMERA_ISO:
                opts->camera_iso = optarg;
                break;
            case OPT_CAMERA_SHUTTER:
                opts->camera_shutter_us = optarg;
                break;
            case OPT_CAMERA_FOCUS:
                opts->camera_focus = optarg;
                break;
            case OPT_CAMERA_AWB:
                opts->camera_awb = optarg;
                break;
"""),
])
print("scrcpy camera patch applied")
