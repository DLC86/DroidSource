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
                            } else if (CAMERA_WB_LOCK_OPTION.equals(optionKey)
                                    && valueObj instanceof Integer) {
                                options.cameraWbLock = (Integer) valueObj != 0;
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
                    boolean manualWb = false;
                    boolean manualPostProcessing = false;
                    if (capabilitiesForWb != null) {
                        for (int capability : capabilitiesForWb) {
                            if (capability == CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING) {
                                manualPostProcessing = true;
                                break;
                            }
                        }
                    }
                    boolean awbOff = false;
                    if (awbModesForWb != null) {
                        for (int mode : awbModesForWb) {
                            if (mode == android.hardware.camera2.CaptureRequest.CONTROL_AWB_MODE_OFF) {
                                awbOff = true;
                                break;
                            }
                        }
                    }
                    boolean transformMatrix = Build.VERSION.SDK_INT < 36;
                    if (Build.VERSION.SDK_INT >= 36 && correctionModesForWb != null) {
                        for (int mode : correctionModesForWb) {
                            if (mode == android.hardware.camera2.CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX) {
                                transformMatrix = true;
                                break;
                            }
                        }
                    }
                    manualWb = manualPostProcessing && awbOff && transformMatrix;
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

                    if (Build.VERSION.SDK_INT >= 36) {
                        int[] aePriorityModes =
                                characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_PRIORITY_MODES);
                        if (aePriorityModes != null) {
                            builder.append(", ae-priority-modes=[");
                            for (int i = 0; i < aePriorityModes.length; ++i) {
                                if (i > 0) {
                                    builder.append(", ");
                                }
                                switch (aePriorityModes[i]) {
                                    case android.hardware.camera2.CaptureRequest.CONTROL_AE_PRIORITY_MODE_OFF:
                                        builder.append("OFF");
                                        break;
                                    case android.hardware.camera2.CaptureRequest.CONTROL_AE_PRIORITY_MODE_SENSOR_SENSITIVITY_PRIORITY:
                                        builder.append("ISO");
                                        break;
                                    case android.hardware.camera2.CaptureRequest.CONTROL_AE_PRIORITY_MODE_SENSOR_EXPOSURE_TIME_PRIORITY:
                                        builder.append("SHUTTER");
                                        break;
                                    default:
                                        builder.append(aePriorityModes[i]);
                                        break;
                                }
                            }
                            builder.append(']');
                        }
                    }

                    if (Build.VERSION.SDK_INT >= 33) {
                        int[] capabilitiesForHdr =
                                characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
                        boolean tenBit = false;
                        if (capabilitiesForHdr != null) {
                            for (int capability : capabilitiesForHdr) {
                                if (capability == CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_DYNAMIC_RANGE_TEN_BIT) {
                                    tenBit = true;
                                    break;
                                }
                            }
                        }
                        if (tenBit) {
                            builder.append(", dynamic-range-10bit=true");
                        }

                        android.hardware.camera2.params.DynamicRangeProfiles dynamicRangeProfiles =
                                characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES);
                        if (dynamicRangeProfiles != null) {
                            builder.append(", dynamic-range-profiles=[");
                            boolean firstProfile = true;
                            for (Long profile : dynamicRangeProfiles.getSupportedProfiles()) {
                                if (!firstProfile) {
                                    builder.append(", ");
                                }
                                firstProfile = false;
                                if (profile == android.hardware.camera2.params.DynamicRangeProfiles.STANDARD) {
                                    builder.append("STANDARD");
                                } else if (profile == android.hardware.camera2.params.DynamicRangeProfiles.HLG10) {
                                    builder.append("HLG10");
                                } else if (profile == android.hardware.camera2.params.DynamicRangeProfiles.HDR10) {
                                    builder.append("HDR10");
                                } else if (profile == android.hardware.camera2.params.DynamicRangeProfiles.HDR10_PLUS) {
                                    builder.append("HDR10_PLUS");
                                } else if (profile == android.hardware.camera2.params.DynamicRangeProfiles.DOLBY_VISION_10B_HDR_REF) {
                                    builder.append("DOLBY_VISION_10B_HDR_REF");
                                } else if (profile == android.hardware.camera2.params.DynamicRangeProfiles.DOLBY_VISION_10B_HDR_REF_PO) {
                                    builder.append("DOLBY_VISION_10B_HDR_REF_PO");
                                } else if (profile == android.hardware.camera2.params.DynamicRangeProfiles.DOLBY_VISION_10B_HDR_OEM) {
                                    builder.append("DOLBY_VISION_10B_HDR_OEM");
                                } else if (profile == android.hardware.camera2.params.DynamicRangeProfiles.DOLBY_VISION_10B_HDR_OEM_PO) {
                                    builder.append("DOLBY_VISION_10B_HDR_OEM_PO");
                                } else if (profile == android.hardware.camera2.params.DynamicRangeProfiles.DOLBY_VISION_8B_HDR_REF) {
                                    builder.append("DOLBY_VISION_8B_HDR_REF");
                                } else if (profile == android.hardware.camera2.params.DynamicRangeProfiles.DOLBY_VISION_8B_HDR_REF_PO) {
                                    builder.append("DOLBY_VISION_8B_HDR_REF_PO");
                                } else if (profile == android.hardware.camera2.params.DynamicRangeProfiles.DOLBY_VISION_8B_HDR_OEM) {
                                    builder.append("DOLBY_VISION_8B_HDR_OEM");
                                } else if (profile == android.hardware.camera2.params.DynamicRangeProfiles.DOLBY_VISION_8B_HDR_OEM_PO) {
                                    builder.append("DOLBY_VISION_8B_HDR_OEM_PO");
                                } else {
                                    builder.append(profile);
                                }
                            }
                            builder.append(']');
                        }
                    }

                    if (Build.VERSION.SDK_INT >= 34) {
                        android.hardware.camera2.params.ColorSpaceProfiles colorSpaceProfiles =
                                characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_COLOR_SPACE_PROFILES);
                        if (colorSpaceProfiles != null) {
                            try {
                                java.util.Set<android.graphics.ColorSpace.Named> colorSpaces =
                                        colorSpaceProfiles.getSupportedColorSpacesForDynamicRange(
                                                android.graphics.ImageFormat.PRIVATE,
                                                android.hardware.camera2.params.DynamicRangeProfiles.STANDARD);
                                builder.append(", standard-color-spaces=[");
                                boolean firstColorSpace = true;
                                for (android.graphics.ColorSpace.Named colorSpace : colorSpaces) {
                                    if (!firstColorSpace) {
                                        builder.append(", ");
                                    }
                                    firstColorSpace = false;
                                    builder.append(colorSpace.name());
                                }
                                builder.append(']');

                                long[] hdrProfiles = {
                                        android.hardware.camera2.params.DynamicRangeProfiles.HLG10,
                                        android.hardware.camera2.params.DynamicRangeProfiles.HDR10,
                                        android.hardware.camera2.params.DynamicRangeProfiles.HDR10_PLUS
                                };
                                String[] hdrNames = {"HLG10", "HDR10", "HDR10_PLUS"};
                                for (int i = 0; i < hdrProfiles.length; ++i) {
                                    try {
                                        java.util.Set<android.graphics.ColorSpace.Named> hdrColorSpaces =
                                                colorSpaceProfiles.getSupportedColorSpacesForDynamicRange(
                                                        android.graphics.ImageFormat.PRIVATE, hdrProfiles[i]);
                                        builder.append(", ").append(hdrNames[i]).append("-color-spaces=[");
                                        boolean firstHdrColorSpace = true;
                                        for (android.graphics.ColorSpace.Named colorSpace : hdrColorSpaces) {
                                            if (!firstHdrColorSpace) {
                                                builder.append(", ");
                                            }
                                            firstHdrColorSpace = false;
                                            builder.append(colorSpace.name());
                                        }
                                        builder.append(']');
                                    } catch (IllegalArgumentException e) {
                                        Ln.w("Could not get supported " + hdrNames[i]
                                                + " camera color spaces for " + id, e);
                                    }
                                }
                            } catch (IllegalArgumentException e) {
                                Ln.w("Could not get supported standard camera color spaces for " + id, e);
                            }
                        }
                    }

                    int[] tonemapModes = characteristics.get(CameraCharacteristics.TONEMAP_AVAILABLE_TONE_MAP_MODES);
                    boolean toneMapGamma = false;
                    boolean toneMapRec709 = false;
                    boolean toneMapContrast = false;
                    if (tonemapModes != null) {
                        for (int mode : tonemapModes) {
                            if (mode == android.hardware.camera2.CaptureRequest.TONEMAP_MODE_GAMMA_VALUE) {
                                toneMapGamma = true;
                            } else if (mode == android.hardware.camera2.CaptureRequest.TONEMAP_MODE_PRESET_CURVE) {
                                toneMapRec709 = true;
                            } else if (mode == android.hardware.camera2.CaptureRequest.TONEMAP_MODE_CONTRAST_CURVE) {
                                toneMapContrast = true;
                            }
                        }
                    }
                    if (toneMapGamma) {
                        builder.append(", tonemap-gamma=true");
                    }
                    if (toneMapRec709) {
                        builder.append(", tonemap-rec709=true");
                    }
                    if (toneMapContrast) {
                        builder.append(", tonemap-contrast=true");
                    }
                    if (toneMapRec709) {
                        // PRESET_CURVE supports both the Rec.709 and sRGB
                        // standards on Camera2; expose this explicitly to the client.
                        builder.append(", tonemap-srgb=true");
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
        """import android.hardware.camera2.TotalCaptureResult;
import android.os.Build;
import android.hardware.camera2.params.ColorSpaceTransform;
import android.hardware.camera2.params.OutputConfiguration;
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
    private boolean cameraWbLock;
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
    private ColorSpaceTransform lastAutoColorCorrectionTransform;
    private RggbChannelVector lastAutoColorCorrectionGains;

    private AffineMatrix transform;
""",
    ),
    (
        """        this.initialTorch = options.getCameraTorch();
        this.zoom = options.getCameraZoom();
""",
        """        this.initialTorch = options.getCameraTorch();
        this.cameraControlPort = options.getCameraControlPort();
        this.cameraWbLock = options.getCameraWbLock() && options.getCameraInitialWbKelvin() <= 0;
        this.manualIso = Math.max(0, options.getCameraInitialIso());
        this.manualShutterUs = Math.max(0, options.getCameraInitialShutterUs());
        this.manualFocusDistance = Math.max(0, options.getCameraInitialFocusDistance());
        this.whiteBalanceKelvin = Math.max(0, options.getCameraInitialWbKelvin());
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
        """            @Override
            public void onCaptureFailed(CameraCaptureSession session, CaptureRequest request, CaptureFailure failure) {
                Ln.w("Camera capture failed: frame " + failure.getFrameNumber());
            }""",
        """            @Override
            public void onCaptureCompleted(CameraCaptureSession session, CaptureRequest request,
                                           TotalCaptureResult result) {
                Integer awbMode = request.get(CaptureRequest.CONTROL_AWB_MODE);
                String activePhysicalId = getActivePhysicalCameraId(result);
                if (awbMode != null
                        && awbMode == CaptureRequest.CONTROL_AWB_MODE_OFF
                        && cameraColorSpace == 3
                        && lockedPhysicalCameraId != null
                        && activePhysicalId != null
                        && !lockedPhysicalCameraId.equals(activePhysicalId)
                        && requestBuilder != null) {
                    Ln.i("Camera physical lens changed from " + lockedPhysicalCameraId
                            + " to " + activePhysicalId + "; refreshing color transform");
                    lockedPhysicalCameraId = null;
                    lastAutoColorCorrectionTransform = null;
                    lastAutoColorCorrectionGains = null;
                    requestBuilder.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
                    requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO);
                    requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE, CaptureRequest.COLOR_CORRECTION_MODE_FAST);
                    requestBuilder.set(CaptureRequest.COLOR_CORRECTION_TRANSFORM, null);
                    requestBuilder.set(CaptureRequest.COLOR_CORRECTION_GAINS, null);
                    try {
                        CaptureRequest updatedRequest = requestBuilder.build();
                        setRepeatingRequest(session, updatedRequest);
                    } catch (CameraAccessException | IllegalArgumentException | IllegalStateException e) {
                        Ln.w("Camera error while refreshing physical-lens color transform: " + e.getMessage());
                    }
                    return;
                }
                if (awbMode != null && awbMode != CaptureRequest.CONTROL_AWB_MODE_OFF) {
                    ColorSpaceTransform transform =
                            result.get(TotalCaptureResult.COLOR_CORRECTION_TRANSFORM);
                    RggbChannelVector gains =
                            result.get(TotalCaptureResult.COLOR_CORRECTION_GAINS);
                    if (transform != null) {
                        lastAutoColorCorrectionTransform = transform;
                        if (gains != null) {
                            lastAutoColorCorrectionGains = gains;
                        }
                        if (whiteBalanceKelvin > 0
                                && cameraCharacteristics != null
                                && requestBuilder != null) {
                            try {
                                applyWhiteBalance();
                                if (activePhysicalId != null) {
                                    lockedPhysicalCameraId = activePhysicalId;
                                }
                                CaptureRequest updatedRequest = requestBuilder.build();
                                setRepeatingRequest(session, updatedRequest);
                            } catch (CameraAccessException | IllegalArgumentException | IllegalStateException e) {
                                Ln.w("Camera error while applying deferred white balance: "
                                        + e.getMessage());
                            }
                        }
                    }
                }
            }

            @Override
            public void onCaptureFailed(CameraCaptureSession session, CaptureRequest request, CaptureFailure failure) {
                Ln.w("Camera capture failed: frame " + failure.getFrameNumber());
            }""",
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
                                  float focusDistance, int wbKelvin, boolean wbLock) {
        cameraHandler.post(() -> {
            assertCameraThread();
            zoom = zoomValue;
            torchEnabled = torch;
            manualIso = Math.max(0, iso);
            manualShutterUs = Math.max(0, shutterUs);
            manualFocusDistance = Math.max(0, focusDistance);
            whiteBalanceKelvin = Math.max(0, wbKelvin);
            cameraWbLock = wbLock && whiteBalanceKelvin <= 0;

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

        int[] edgeModes = cameraCharacteristics.get(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES);
        if (contains(edgeModes, CaptureRequest.EDGE_MODE_OFF)) {
            requestBuilder.set(CaptureRequest.EDGE_MODE, CaptureRequest.EDGE_MODE_OFF);
        }

        int[] noiseReductionModes =
                cameraCharacteristics.get(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES);
        if (contains(noiseReductionModes, CaptureRequest.NOISE_REDUCTION_MODE_OFF)) {
            requestBuilder.set(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_OFF);
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
        requestBuilder.set(CaptureRequest.CONTROL_POST_RAW_SENSITIVITY_BOOST, null);
        requestBuilder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, null);
        requestBuilder.set(CaptureRequest.SENSOR_FRAME_DURATION, null);
    }

    private void applyExposure() {
        assertCameraThread();

        boolean hasIso = manualIso > 0;
        boolean hasShutter = manualShutterUs > 0;

        // A fixed [fps,fps] AE target can conflict with an exposure longer
        // than one frame period. In that case let Camera2 reduce frame rate.
        if (fps > 0 && hasShutter) {
            long requestedExposureNs = manualShutterUs * 1000L;
            long nominalFrameNs = 1_000_000_000L / fps;
            if (requestedExposureNs > nominalFrameNs) {
                requestBuilder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, null);
            } else {
                requestBuilder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,
                        new Range<>(fps, fps));
            }
        } else if (fps > 0) {
            requestBuilder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,
                    new Range<>(fps, fps));
        }

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

            long targetIso = Math.max(1L, manualIso);
            int sensorIso = isoRange.clamp((int)Math.min(targetIso, (long)isoRange.getUpper()));

            Range<Integer> postRawBoostRange = cameraCharacteristics.get(
                    CameraCharacteristics.CONTROL_POST_RAW_SENSITIVITY_BOOST_RANGE);
            int postRawBoost = 100;
            if (postRawBoostRange != null) {
                postRawBoost = postRawBoostRange.clamp(100);
                if (targetIso > sensorIso && sensorIso > 0) {
                    long requiredBoost = Math.round((double)targetIso * 100.0 / sensorIso);
                    int requestedBoost = requiredBoost > Integer.MAX_VALUE
                            ? Integer.MAX_VALUE : (int)requiredBoost;
                    postRawBoost = postRawBoostRange.clamp(requestedBoost);
                }
            }

            long exposureNs = exposureRange.clamp(manualShutterUs * 1000L);
            requestBuilder.set(CaptureRequest.CONTROL_AE_MODE,
                    CaptureRequest.CONTROL_AE_MODE_OFF);
            if (android.os.Build.VERSION.SDK_INT >= 36) {
                requestBuilder.set(CaptureRequest.CONTROL_AE_PRIORITY_MODE,
                        CaptureRequest.CONTROL_AE_PRIORITY_MODE_OFF);
            }
            requestBuilder.set(CaptureRequest.SENSOR_SENSITIVITY, sensorIso);
            requestBuilder.set(CaptureRequest.CONTROL_POST_RAW_SENSITIVITY_BOOST, postRawBoost);
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
            requestBuilder.set(CaptureRequest.CONTROL_MODE,
                    CaptureRequest.CONTROL_MODE_AUTO);
            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                    CaptureRequest.CONTROL_AWB_MODE_AUTO);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_GAINS, null);
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
                requestBuilder.set(CaptureRequest.CONTROL_MODE,
                        CaptureRequest.CONTROL_MODE_AUTO);
                requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                        CaptureRequest.CONTROL_AWB_MODE_OFF);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE,
                        CaptureRequest.COLOR_CORRECTION_MODE_CCT);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TEMPERATURE,
                        cctRange.clamp(whiteBalanceKelvin));
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TINT, 0);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_GAINS, null);
                Ln.i("Camera white balance: native CCT " + whiteBalanceKelvin + " K");
                return;
            }
        }

        int[] capabilities = cameraCharacteristics.get(
                CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
        int[] awbModes = cameraCharacteristics.get(
                CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES);
        int[] correctionModes = cameraCharacteristics.get(
                CameraCharacteristics.COLOR_CORRECTION_AVAILABLE_MODES);

        boolean manualPostProcessing = contains(capabilities,
                CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING);
        boolean awbOff = contains(awbModes, CaptureRequest.CONTROL_AWB_MODE_OFF);

        // COLOR_CORRECTION_AVAILABLE_MODES was introduced in API 36.
        // Before API 36, MANUAL_POST_PROCESSING + AWB OFF is sufficient for
        // the standard transform/gains path.
        boolean transformMatrix = android.os.Build.VERSION.SDK_INT < 36
                || contains(correctionModes, CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX);

        if (manualPostProcessing && awbOff && transformMatrix) {
            // Reuse the camera's own calibrated sensor-to-sRGB transform
            // reported by an AUTO capture. An identity matrix would bypass
            // the sensor-specific color conversion.
            if (lastAutoColorCorrectionTransform == null
                    || lastAutoColorCorrectionGains == null) {
                // Manual WB may be requested before the first frame is returned.
                // Let AUTO produce one result; the capture callback will then
                // re-apply the requested manual WB using the real transform.
                requestBuilder.set(CaptureRequest.CONTROL_MODE,
                        CaptureRequest.CONTROL_MODE_AUTO);
                requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                        CaptureRequest.CONTROL_AWB_MODE_AUTO);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE,
                        CaptureRequest.COLOR_CORRECTION_MODE_FAST);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_TRANSFORM, null);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_GAINS, null);
                if (android.os.Build.VERSION.SDK_INT >= 36) {
                    requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TEMPERATURE, null);
                    requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TINT, null);
                }
                Ln.i("Camera white balance: waiting for AUTO color transform");
                return;
            }

            requestBuilder.set(CaptureRequest.CONTROL_MODE,
                    CaptureRequest.CONTROL_MODE_AUTO);
            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                    CaptureRequest.CONTROL_AWB_MODE_OFF);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE,
                    CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_TRANSFORM,
                    getTargetColorTransform());

            RggbChannelVector gains = whiteBalanceKelvin > 0
                    ? makeManualGainsFromAuto(whiteBalanceKelvin)
                    : lastAutoColorCorrectionGains;
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_GAINS, gains);
            if (android.os.Build.VERSION.SDK_INT >= 36) {
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TEMPERATURE, null);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TINT, null);
            }
            Ln.i("Camera white balance: manual gains "
                    + whiteBalanceKelvin + " K relative to AUTO -> " + gains);
            return;
        }

        requestBuilder.set(CaptureRequest.CONTROL_MODE,
                CaptureRequest.CONTROL_MODE_AUTO);
        requestBuilder.set(CaptureRequest.COLOR_CORRECTION_GAINS, null);
        if (android.os.Build.VERSION.SDK_INT >= 36) {
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE,
                    CaptureRequest.COLOR_CORRECTION_MODE_FAST);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TEMPERATURE, null);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_COLOR_TINT, null);
        }

        int wbMode = chooseAwbMode(whiteBalanceKelvin, awbModes);
        if (wbMode != CaptureRequest.CONTROL_AWB_MODE_AUTO) {
            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE, wbMode);
            Ln.i("Camera white balance: AWB preset " + wbMode + " for " + whiteBalanceKelvin + " K");
        } else {
            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                    CaptureRequest.CONTROL_AWB_MODE_AUTO);
            Ln.w("Camera does not advertise manual white balance control");
        }
    }

    private RggbChannelVector makeManualGainsFromAuto(int requestedKelvin) {
        assertCameraThread();

        if (lastAutoColorCorrectionGains == null) {
            return kelvinToGains(requestedKelvin);
        }

        double autoR = Math.max(1e-6, lastAutoColorCorrectionGains.getRed());
        double autoGe = Math.max(1e-6, lastAutoColorCorrectionGains.getGreenEven());
        double autoGo = Math.max(1e-6, lastAutoColorCorrectionGains.getGreenOdd());
        double autoB = Math.max(1e-6, lastAutoColorCorrectionGains.getBlue());

        double autoKelvin = estimateKelvinFromGains(autoR, (autoGe + autoGo) * 0.5, autoB);
        RggbChannelVector target = kelvinToGains(requestedKelvin);
        RggbChannelVector reference = kelvinToGains((int) Math.round(autoKelvin));

        // Apply only the chromatic change from AUTO to the requested Kelvin.
        // The AUTO green gain is retained, which keeps luminance close to the
        // camera's own exposure at the reference white point.
        double targetR = target.getRed() / Math.max(1e-6, reference.getRed());
        double targetG = target.getGreenEven() / Math.max(1e-6, reference.getGreenEven());
        double targetB = target.getBlue() / Math.max(1e-6, reference.getBlue());

        float r = clampGain((float) (autoR * targetR / targetG));
        float ge = clampGain((float) (autoGe));
        float go = clampGain((float) (autoGo));
        float b = clampGain((float) (autoB * targetB / targetG));

        return new RggbChannelVector(r, ge, go, b);
    }
    private static float clampGain(float value) {
        return (float) Math.max(1.0, Math.min(3.0, value));
    }

    private static double estimateKelvinFromGains(double red, double green, double blue) {
        double bestKelvin = 5500.0;
        double bestError = Double.POSITIVE_INFINITY;

        for (int kelvin = 1500; kelvin <= 12000; kelvin += 50) {
            RggbChannelVector candidate = kelvinToGains(kelvin);
            double candidateR = Math.max(1e-6, candidate.getRed());
            double candidateB = Math.max(1e-6, candidate.getBlue());
            double errorR = Math.log(red / green) - Math.log(candidateR / candidate.getGreenEven());
            double errorB = Math.log(blue / green) - Math.log(candidateB / candidate.getGreenEven());
            double error = errorR * errorR + errorB * errorB;
            if (error < bestError) {
                bestError = error;
                bestKelvin = kelvin;
            }
        }

        return bestKelvin;
    }

    private static RggbChannelVector kelvinToGains(int kelvin) {
        double temperature = Math.max(1000, Math.min(15000, kelvin)) / 100.0;
        double red;
        double green;
        double blue;

        if (temperature <= 66.0) {
            red = 255.0;
            green = 99.4708025861 * Math.log(Math.max(1.0, temperature)) - 161.1195681661;
        } else {
            red = 329.698727446 * Math.pow(temperature - 60.0, -0.1332047592);
            green = 288.1221695283 * Math.pow(temperature - 60.0, -0.0755148492);
        }

        if (temperature >= 66.0) {
            blue = 255.0;
        } else if (temperature <= 19.0) {
            blue = 1.0;
        } else {
            blue = 138.5177312231 * Math.log(temperature - 10.0) - 305.0447927303;
        }

        red = Math.max(1.0, Math.min(255.0, red));
        green = Math.max(1.0, Math.min(255.0, green));
        blue = Math.max(1.0, Math.min(255.0, blue));

        float redGain = (float)Math.max(1.0, Math.min(8.0, green / red));
        float blueGain = (float)Math.max(1.0, Math.min(8.0, green / blue));
        return new RggbChannelVector(redGain, 1.0f, 1.0f, blueGain);
    }

    private static int chooseAwbMode(int kelvin, int[] awbModes) {
        if (awbModes == null) {
            return CaptureRequest.CONTROL_AWB_MODE_AUTO;
        }

        final int[][] candidates = {
                {CaptureRequest.CONTROL_AWB_MODE_INCANDESCENT, 2800},
                {CaptureRequest.CONTROL_AWB_MODE_WARM_FLUORESCENT, 3200},
                {CaptureRequest.CONTROL_AWB_MODE_FLUORESCENT, 4000},
                {CaptureRequest.CONTROL_AWB_MODE_DAYLIGHT, 5500},
                {CaptureRequest.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT, 6500},
                {CaptureRequest.CONTROL_AWB_MODE_TWILIGHT, 7500},
                {CaptureRequest.CONTROL_AWB_MODE_SHADE, 8000},
        };

        int bestMode = CaptureRequest.CONTROL_AWB_MODE_AUTO;
        int bestDistance = Integer.MAX_VALUE;
        for (int[] candidate : candidates) {
            if (!contains(awbModes, candidate[0])) {
                continue;
            }
            int distance = Math.abs(kelvin - candidate[1]);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestMode = candidate[0];
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


# Additional color-management and 10-bit patches.
def patch_generated(path, replacements):
    p = ROOT / path
    s = p.read_text(encoding="utf-8")
    for old, new in replacements:
        if old not in s:
            raise SystemExit(f"Generated patch pattern not found in {path}: {old[:200]!r}")
        s = s.replace(old, new, 1)
    p.write_text(s, encoding="utf-8")


patch_generated("server/src/main/java/com/genymobile/scrcpy/Options.java", [
    (
        '    private static final String CAMERA_CONTROL_OPTION = "__scrcpy_obs_camera_control_port";\n',
        '    private static final String CAMERA_CONTROL_OPTION = "__scrcpy_obs_camera_control_port";\n'
        '    private static final String CAMERA_WB_LOCK_OPTION = "__scrcpy_obs_camera_wb_lock";\n'
        '    private static final String CAMERA_COLOR_SPACE_OPTION = "__scrcpy_obs_camera_color_space";\n'
        '    private static final String CAMERA_GAMMA_OPTION = "__scrcpy_obs_camera_gamma";\n'
        '    private static final String CAMERA_10BIT_OPTION = "__scrcpy_obs_camera_10bit";\n'
        '    private static final String CAMERA_10BIT_TO_8BIT_OPTION = "__scrcpy_obs_camera_10bit_to_8bit";\n'
        '    private static final String CAMERA_ISO_OPTION = "__scrcpy_obs_camera_iso";\n'
        '    private static final String CAMERA_SHUTTER_OPTION = "__scrcpy_obs_camera_shutter_us";\n'
        '    private static final String CAMERA_FOCUS_OPTION = "__scrcpy_obs_camera_focus_distance";\n'
        '    private static final String CAMERA_WB_KELVIN_OPTION = "__scrcpy_obs_camera_wb_kelvin";\n',
    ),
    (
        "    private int cameraControlPort;\n    private boolean showTouches;\n",
        "    private int cameraControlPort;\n    private boolean cameraWbLock;\n    private int cameraColorSpace;\n    private int cameraGamma;\n    private boolean camera10Bit;\n    private boolean camera10BitTo8Bit;\n    private int cameraInitialIso;\n    private int cameraInitialShutterUs;\n    private float cameraInitialFocusDistance;\n    private int cameraInitialWbKelvin;\n    private boolean showTouches;\n",
    ),
    (
        """    public int getCameraControlPort() {
        return cameraControlPort;
    }

    public boolean getShowTouches() {
""",
        """    public int getCameraControlPort() {
        return cameraControlPort;
    }

    public boolean getCameraWbLock() {
        return cameraWbLock;
    }

    public int getCameraColorSpace() {
        return cameraColorSpace;
    }

    public int getCameraGamma() {
        return cameraGamma;
    }

    public boolean getCamera10Bit() {
        return camera10Bit;
    }

    public boolean getCamera10BitTo8Bit() {
        return camera10BitTo8Bit;
    }

    public int getCameraInitialIso() {
        return cameraInitialIso;
    }

    public int getCameraInitialShutterUs() {
        return cameraInitialShutterUs;
    }

    public float getCameraInitialFocusDistance() {
        return cameraInitialFocusDistance;
    }

    public int getCameraInitialWbKelvin() {
        return cameraInitialWbKelvin;
    }

    public boolean getShowTouches() {
""",
    ),
    (
        """                            if (CAMERA_CONTROL_OPTION.equals(optionKey)
                                    && valueObj instanceof Integer) {
                                int port = (Integer) valueObj;
                                if (port < 1 || port > 65535) {
                                    throw new IllegalArgumentException("Invalid camera control port: " + port);
                                }
                                options.cameraControlPort = port;
                                codecOptions.remove(j);
                            } else if (CAMERA_WB_LOCK_OPTION.equals(optionKey)
                                    && valueObj instanceof Integer) {
                                options.cameraWbLock = (Integer) valueObj != 0;
                                codecOptions.remove(j);
                            } else {
                                ++j;
                            }
""",
        """                            if (CAMERA_CONTROL_OPTION.equals(optionKey)
                                    && valueObj instanceof Integer) {
                                int port = (Integer) valueObj;
                                if (port < 1 || port > 65535) {
                                    throw new IllegalArgumentException("Invalid camera control port: " + port);
                                }
                                options.cameraControlPort = port;
                                codecOptions.remove(j);
                            } else if (CAMERA_WB_LOCK_OPTION.equals(optionKey)
                                    && valueObj instanceof Integer) {
                                options.cameraWbLock = (Integer) valueObj != 0;
                                codecOptions.remove(j);
                            } else if (CAMERA_COLOR_SPACE_OPTION.equals(optionKey)
                                    && valueObj instanceof Integer) {
                                int colorSpace = (Integer) valueObj;
                                if (colorSpace < 0 || colorSpace > 3) {
                                    throw new IllegalArgumentException("Invalid camera color space: " + colorSpace);
                                }
                                options.cameraColorSpace = colorSpace;
                                codecOptions.remove(j);
                            } else if (CAMERA_GAMMA_OPTION.equals(optionKey)
                                    && valueObj instanceof Integer) {
                                int gamma = (Integer) valueObj;
                                if (gamma < 0 || gamma > 8) {
                                    throw new IllegalArgumentException("Invalid camera gamma: " + gamma);
                                }
                                options.cameraGamma = gamma;
                                codecOptions.remove(j);
                            } else if (CAMERA_10BIT_OPTION.equals(optionKey)
                                    && valueObj instanceof Integer) {
                                options.camera10Bit = (Integer) valueObj != 0;
                                codecOptions.remove(j);
                            } else if (CAMERA_10BIT_TO_8BIT_OPTION.equals(optionKey)
                                    && valueObj instanceof Integer) {
                                options.camera10BitTo8Bit = (Integer) valueObj != 0;
                                codecOptions.remove(j);
                            } else if (CAMERA_ISO_OPTION.equals(optionKey)
                                    && valueObj instanceof Integer) {
                                int iso = (Integer) valueObj;
                                if (iso < 0) {
                                    throw new IllegalArgumentException("Invalid initial camera ISO: " + iso);
                                }
                                options.cameraInitialIso = iso;
                                codecOptions.remove(j);
                            } else if (CAMERA_SHUTTER_OPTION.equals(optionKey)
                                    && valueObj instanceof Integer) {
                                int shutterUs = (Integer) valueObj;
                                if (shutterUs < 0) {
                                    throw new IllegalArgumentException("Invalid initial camera shutter: " + shutterUs);
                                }
                                options.cameraInitialShutterUs = shutterUs;
                                codecOptions.remove(j);
                            } else if (CAMERA_FOCUS_OPTION.equals(optionKey)
                                    && valueObj instanceof Float) {
                                float focusDistance = (Float) valueObj;
                                if (focusDistance < 0) {
                                    throw new IllegalArgumentException("Invalid initial camera focus distance: " + focusDistance);
                                }
                                options.cameraInitialFocusDistance = focusDistance;
                                codecOptions.remove(j);
                            } else if (CAMERA_WB_KELVIN_OPTION.equals(optionKey)
                                    && valueObj instanceof Integer) {
                                int wbKelvin = (Integer) valueObj;
                                if (wbKelvin < 0) {
                                    throw new IllegalArgumentException("Invalid initial camera white balance: " + wbKelvin);
                                }
                                options.cameraInitialWbKelvin = wbKelvin;
                                codecOptions.remove(j);
                            } else {
                                ++j;
                            }
""",
    ),
])


patch_generated("server/src/main/java/com/genymobile/scrcpy/video/SurfaceEncoder.java", [
    (
        """        Codec codec = streamer.getCodec();
        MediaCodec mediaCodec = createMediaCodec(codec, encoderName);
        MediaFormat format = createFormat(codec.getMimeType(), videoBitRate, maxFps, codecOptions);
""",
        """        Codec codec = streamer.getCodec();
        MediaCodec mediaCodec = createMediaCodec(codec, encoderName);
        boolean camera10Bit = tenBit;
        int cameraColorSpace = camera10Bit ? 3 : this.cameraColorSpace;
        int cameraGamma = camera10Bit
                ? (this.cameraGamma >= 5 && this.cameraGamma <= 7 ? this.cameraGamma : 5)
                : this.cameraGamma;
        if (camera10Bit && !MediaFormat.MIMETYPE_VIDEO_HEVC.equals(codec.getMimeType())) {
            throw new ConfigurationException("Camera 10-bit requires HEVC");
        }
        MediaFormat format = createFormat(codec.getMimeType(), videoBitRate, maxFps, codecOptions,
                                           camera10Bit, cameraColorSpace, cameraGamma);
""",
    ),
    (
        """    private static MediaFormat createFormat(String videoMimeType, int bitRate, float maxFps, List<CodecOption> codecOptions) {
""",
        """    private static MediaFormat createFormat(String videoMimeType, int bitRate, float maxFps,
                                             List<CodecOption> codecOptions, boolean tenBit,
                                             int cameraColorSpace, int cameraGamma)
            throws ConfigurationException {
""",
    ),
    (
        """        format.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        if (Build.VERSION.SDK_INT >= AndroidVersions.API_24_ANDROID_7_0) {
""",
        """        format.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        if (Build.VERSION.SDK_INT >= AndroidVersions.API_24_ANDROID_7_0) {
            /*
             * Propagate camera colorimetry into the encoder. This is needed for
             * the OBS/FFmpeg side to interpret explicit BT.2020 correctly.
             */
            if (tenBit) {
                if (Build.VERSION.SDK_INT < AndroidVersions.API_33_ANDROID_13) {
                    throw new ConfigurationException("Camera 10-bit requires Android 13 or newer");
                }
                int profile;
                int transfer;
                if (cameraGamma == 6) {
                    profile = MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10HDR10;
                    transfer = MediaFormat.COLOR_TRANSFER_ST2084;
                } else if (cameraGamma == 7) {
                    profile = MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10HDR10Plus;
                    transfer = MediaFormat.COLOR_TRANSFER_ST2084;
                } else {
                    profile = MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10;
                    transfer = MediaFormat.COLOR_TRANSFER_HLG;
                }
                format.setInteger(MediaFormat.KEY_PROFILE, profile);
                format.setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT2020);
                format.setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED);
                format.setInteger(MediaFormat.KEY_COLOR_TRANSFER, transfer);
            } else if (cameraColorSpace == 1 || cameraColorSpace == 2 || cameraColorSpace == 3) {
                int standard = cameraColorSpace == 3
                        ? MediaFormat.COLOR_STANDARD_BT2020
                        : MediaFormat.COLOR_STANDARD_BT709;
                format.setInteger(MediaFormat.KEY_COLOR_STANDARD, standard);
                format.setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED);

                int transfer = 0;
                if (cameraGamma == 1) {
                    // H.264/HEVC transfer_characteristics value 4 = Gamma 2.2.
                    transfer = 4;
                } else if (cameraGamma == 2 || cameraGamma == 3) {
                    transfer = MediaFormat.COLOR_TRANSFER_SDR_VIDEO;
                } else if (cameraGamma == 4 || cameraGamma == 8) {
                    // sRGB and Rec.709-A are both represented by the sRGB
                    // transfer id here; Rec.709-A has no dedicated MediaFormat
                    // transfer constant.
                    transfer = 2;
                } else if (cameraGamma == 5) {
                    transfer = MediaFormat.COLOR_TRANSFER_HLG;
                } else if (cameraColorSpace == 3) {
                    // Android's BT.2020 named space uses a 2.2 OETF.
                    transfer = 4;
                } else if (cameraColorSpace == 1) {
                    transfer = 2;
                } else if (cameraColorSpace == 2) {
                    transfer = MediaFormat.COLOR_TRANSFER_SDR_VIDEO;
                }

                if (transfer != 0) {
                    format.setInteger(MediaFormat.KEY_COLOR_TRANSFER, transfer);
                }
            }
        }
        if (Build.VERSION.SDK_INT >= AndroidVersions.API_24_ANDROID_7_0) {
""",
    ),
])


patch_generated("server/src/main/java/com/genymobile/scrcpy/video/SurfaceEncoder.java", [
    (
        """    private final boolean downsizeOnError;
    private final int minSizeAlignment;
""",
        """    private final boolean downsizeOnError;
    private final int minSizeAlignment;
    private final boolean tenBit;
    private final int cameraColorSpace;
    private final int cameraGamma;
""",
    ),
    (
        """        this.downsizeOnError = options.getDownsizeOnError();
        this.minSizeAlignment = options.getMinSizeAlignment();
""",
        """        this.downsizeOnError = options.getDownsizeOnError();
        this.minSizeAlignment = options.getMinSizeAlignment();
        this.tenBit = options.getVideoSource() == VideoSource.CAMERA && options.getCamera10Bit();
        this.cameraColorSpace = options.getVideoSource() == VideoSource.CAMERA
                ? options.getCameraColorSpace() : 0;
        this.cameraGamma = options.getVideoSource() == VideoSource.CAMERA
                ? options.getCameraGamma() : 0;
""",
    ),
])


patch_generated("server/src/main/java/com/genymobile/scrcpy/video/CameraCapture.java", [
    (
        """import android.hardware.camera2.params.ColorSpaceTransform;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.RggbChannelVector;
""",
        """import android.hardware.camera2.params.ColorSpaceProfiles;
import android.hardware.camera2.params.ColorSpaceTransform;
import android.hardware.camera2.params.DynamicRangeProfiles;
import android.hardware.camera2.params.OutputConfiguration;
import android.hardware.camera2.params.RggbChannelVector;
import android.hardware.camera2.params.TonemapCurve;
""",
    ),
    (
        """    private final int cameraControlPort;
    private boolean cameraWbLock;
    private float zoom;
""",
        """    private final int cameraControlPort;
    private boolean cameraWbLock;
    private int cameraColorSpace;
    private int cameraGamma;
    private boolean cameraTenBit;
    private boolean cameraTenBitTo8Bit;
    private String lockedPhysicalCameraId;
    private float zoom;
""",
    ),
    (
        """        this.cameraControlPort = options.getCameraControlPort();
        this.cameraWbLock = options.getCameraWbLock() && options.getCameraInitialWbKelvin() <= 0;
        this.manualIso = Math.max(0, options.getCameraInitialIso());
        this.manualShutterUs = Math.max(0, options.getCameraInitialShutterUs());
        this.manualFocusDistance = Math.max(0, options.getCameraInitialFocusDistance());
        this.whiteBalanceKelvin = Math.max(0, options.getCameraInitialWbKelvin());
        this.zoom = options.getCameraZoom();
""",
        """        this.cameraControlPort = options.getCameraControlPort();
        this.cameraWbLock = options.getCameraWbLock() && options.getCameraInitialWbKelvin() <= 0;
        this.cameraColorSpace = options.getCameraColorSpace();
        this.cameraGamma = options.getCameraGamma();
        this.cameraTenBit = options.getCamera10Bit();
        this.cameraTenBitTo8Bit = options.getCamera10BitTo8Bit();
        this.lockedPhysicalCameraId = null;
        Ln.i("Camera pipeline mode: 10-bit=" + cameraTenBit
                + ", 10-bit-to-8-bit=" + cameraTenBitTo8Bit
                + ", color-space=" + cameraColorSpace
                + ", gamma=" + cameraGamma);
        this.manualIso = Math.max(0, options.getCameraInitialIso());
        this.manualShutterUs = Math.max(0, options.getCameraInitialShutterUs());
        this.manualFocusDistance = Math.max(0, options.getCameraInitialFocusDistance());
        this.whiteBalanceKelvin = Math.max(0, options.getCameraInitialWbKelvin());
        this.zoom = options.getCameraZoom();
""",
    ),
    (
        """        OutputConfiguration outputConfig = new OutputConfiguration(captureSurface);
        List<OutputConfiguration> outputs = Collections.singletonList(outputConfig);
""",
        """        OutputConfiguration outputConfig = new OutputConfiguration(captureSurface);
        if (cameraTenBit) {
            if (Build.VERSION.SDK_INT < AndroidVersions.API_33_ANDROID_13) {
                throw new IOException("Camera 10-bit requires Android 13 or newer");
            }
            if (highSpeed) {
                throw new IOException("Camera 10-bit is not supported for high-speed capture");
            }
            CameraCharacteristics characteristics;
            try {
                characteristics = ServiceManager.getCameraManager().getCameraCharacteristics(cameraId);
            } catch (CameraAccessException e) {
                throw new IOException("Could not get camera characteristics for 10-bit output", e);
            }
            int[] capabilities = characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
            DynamicRangeProfiles profiles =
                    characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES);
            boolean tenBitSupported = contains(
                    capabilities, CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_DYNAMIC_RANGE_TEN_BIT);
            long dynamicRange = DynamicRangeProfiles.HLG10;
            if (cameraGamma == 6) {
                dynamicRange = DynamicRangeProfiles.HDR10;
            } else if (cameraGamma == 7) {
                dynamicRange = DynamicRangeProfiles.HDR10_PLUS;
            }
            boolean dynamicRangeSupported = profiles != null
                    && profiles.getSupportedProfiles().contains(dynamicRange);
            if (!tenBitSupported || !dynamicRangeSupported) {
                throw new IOException("Camera does not support the requested 10-bit HDR profile");
            }
            outputConfig.setDynamicRangeProfile(dynamicRange);
        }
        List<OutputConfiguration> outputs = Collections.singletonList(outputConfig);
""",
    ),
    (
        """    public void setCameraSettings(float zoomValue, boolean torch, int iso, int shutterUs,
                                  float focusDistance, int wbKelvin, boolean wbLock) {
""",
        """    public void setCameraSettings(float zoomValue, boolean torch, int iso, int shutterUs,
                                  float focusDistance, int wbKelvin, boolean wbLock,
                                  int colorSpace, int gamma, boolean tenBit, int dynamicRange) {
""",
    ),
    (
        """            manualFocusDistance = Math.max(0, focusDistance);
            whiteBalanceKelvin = Math.max(0, wbKelvin);
            cameraWbLock = wbLock && whiteBalanceKelvin <= 0;

            if (currentSession != null && requestBuilder != null) {
""",
        """            manualFocusDistance = Math.max(0, focusDistance);
            whiteBalanceKelvin = Math.max(0, wbKelvin);
            cameraWbLock = wbLock && whiteBalanceKelvin <= 0;
            cameraColorSpace = Math.max(0, Math.min(3, colorSpace));
            cameraGamma = Math.max(0, Math.min(8, gamma));
            cameraTenBit = tenBit;

            if (currentSession != null && requestBuilder != null) {
""",
    ),
    (
        """        Boolean flashAvailable =
""",
        """        try {
            applyGamma();
        } catch (RuntimeException e) {
            Ln.w("Could not apply camera gamma: " + e.getMessage());
            requestBuilder.set(CaptureRequest.TONEMAP_MODE, CaptureRequest.TONEMAP_MODE_FAST);
            requestBuilder.set(CaptureRequest.TONEMAP_GAMMA, null);
            requestBuilder.set(CaptureRequest.TONEMAP_CURVE, null);
        }

        Boolean flashAvailable =
""",
    ),
    (
        """        if (android.os.Build.VERSION.SDK_INT >= 36) {
            Range<Integer> cctRange = cameraCharacteristics.get(
""",
        """        if (android.os.Build.VERSION.SDK_INT >= 36 && cameraColorSpace == 0) {
            Range<Integer> cctRange = cameraCharacteristics.get(
""",
    ),
    (
        """        if (manualPostProcessing && awbOff && transformMatrix) {
""",
        """        if (manualPostProcessing && awbOff && transformMatrix
                && (whiteBalanceKelvin > 0 || cameraColorSpace == 3)) {
""",
    ),
    (
        """        });

        try {
            cameraDevice.createCaptureSession(sessionConfig);
""",
        """        });

        if (cameraTenBit || (cameraColorSpace != 0 && Build.VERSION.SDK_INT >= AndroidVersions.API_34_ANDROID_14)) {
            if (Build.VERSION.SDK_INT >= AndroidVersions.API_34_ANDROID_14) {
                CameraCharacteristics characteristics;
                try {
                    characteristics =
                            ServiceManager.getCameraManager().getCameraCharacteristics(cameraId);
                } catch (CameraAccessException e) {
                    throw new IOException("Could not get camera characteristics for color-space selection", e);
                }
                ColorSpaceProfiles profiles =
                        characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_COLOR_SPACE_PROFILES);
                long requestedDynamicRange = DynamicRangeProfiles.STANDARD;
                if (cameraTenBit) {
                    if (cameraGamma == 6) {
                        requestedDynamicRange = DynamicRangeProfiles.HDR10;
                    } else if (cameraGamma == 7) {
                        requestedDynamicRange = DynamicRangeProfiles.HDR10_PLUS;
                    } else {
                        requestedDynamicRange = DynamicRangeProfiles.HLG10;
                    }
                }

                android.graphics.ColorSpace.Named requestedColorSpace = null;
                if (cameraTenBit) {
                    requestedColorSpace = (requestedDynamicRange == DynamicRangeProfiles.HLG10)
                            ? android.graphics.ColorSpace.Named.BT2020_HLG
                            : android.graphics.ColorSpace.Named.BT2020_PQ;
                } else if (cameraColorSpace != 0) {
                    switch (cameraColorSpace) {
                        case 1:
                            requestedColorSpace = android.graphics.ColorSpace.Named.SRGB;
                            break;
                        case 2:
                            requestedColorSpace = android.graphics.ColorSpace.Named.BT709;
                            break;
                        case 3:
                            requestedColorSpace = android.graphics.ColorSpace.Named.BT2020;
                            break;
                        default:
                            break;
                    }
                }
                java.util.Set<android.graphics.ColorSpace.Named> supportedColorSpaces = null;
                java.util.Set<android.graphics.ColorSpace.Named> supportedStandardColorSpaces = null;
                java.util.Set<Long> supportedDynamicRangesForColorSpace = null;
                boolean colorSpaceSupported = false;
                boolean standardProfileSupported = false;
                if (profiles != null && requestedColorSpace != null) {
                    supportedColorSpaces = profiles.getSupportedColorSpaces(
                            android.graphics.ImageFormat.PRIVATE);
                    colorSpaceSupported = supportedColorSpaces.contains(requestedColorSpace);

                    /*
                     * A color-space profile is a combination of color space,
                     * image format and dynamic-range profile. Checking only
                     * getSupportedColorSpaces(PRIVATE) is insufficient: for
                     * example BT.2020 may be available only together with
                     * HLG10, while the 8-bit camera path needs STANDARD.
                     */
                    if (Build.VERSION.SDK_INT >= AndroidVersions.API_34_ANDROID_14) {
                        if (cameraTenBit) {
                            supportedStandardColorSpaces = profiles.getSupportedColorSpacesForDynamicRange(
                                    android.graphics.ImageFormat.PRIVATE, requestedDynamicRange);
                            standardProfileSupported = supportedStandardColorSpaces.contains(requestedColorSpace);
                        } else {
                            supportedStandardColorSpaces = profiles.getSupportedColorSpacesForDynamicRange(
                                    android.graphics.ImageFormat.PRIVATE,
                                    DynamicRangeProfiles.STANDARD);
                            standardProfileSupported = supportedStandardColorSpaces.contains(requestedColorSpace);
                        }
                        supportedDynamicRangesForColorSpace =
                                profiles.getSupportedDynamicRangeProfiles(
                                        requestedColorSpace, android.graphics.ImageFormat.PRIVATE);
                    }
                }

                Ln.i("Requested camera color space=" + (requestedColorSpace == null
                        ? "DEFAULT" : requestedColorSpace.name())
                        + ", supported=" + colorSpaceSupported
                        + ", profile=" + standardProfileSupported
                        + (supportedColorSpaces == null ? "" : ", supported-spaces=" + supportedColorSpaces)
                        + (supportedStandardColorSpaces == null
                                ? "" : ", supported-standard-spaces=" + supportedStandardColorSpaces)
                        + (supportedDynamicRangesForColorSpace == null
                                ? "" : ", dynamic-ranges-for-color-space=" + supportedDynamicRangesForColorSpace));

                if (requestedColorSpace != null && colorSpaceSupported && standardProfileSupported) {
                    sessionConfig.setColorSpace(requestedColorSpace);
                    Ln.i("Camera session color space set to " + requestedColorSpace.name()
                            + " with " + (cameraTenBit ? String.valueOf(requestedDynamicRange) : "STANDARD") + " profile");
                } else if (cameraTenBit) {
                    // The dynamic-range profile itself remains authoritative for HDR.
                    // Some devices do not expose a ColorSpaceProfiles entry for the
                    // corresponding named BT.2020_HLG/PQ space.
                    Ln.i("Using camera HDR dynamic range profile without an explicit named color space: "
                            + requestedDynamicRange);
                } else if (requestedColorSpace != null) {
                    Ln.w("Requested camera color space is not supported for the "
                            + (cameraTenBit ? String.valueOf(requestedDynamicRange) : "STANDARD")
                            + " profile: " + requestedColorSpace);
                }
            } else if (cameraTenBit) {
                throw new IOException("Camera 10-bit requires Android 14 or newer");
            }
        }

        try {
            cameraDevice.createCaptureSession(sessionConfig);
""",
    ),
])


# Insert matrix/tonemap helpers into CameraCapture.
p = ROOT / "server/src/main/java/com/genymobile/scrcpy/video/CameraCapture.java"
s = p.read_text(encoding="utf-8")
marker = """    private void clearManualExposureKeys() {
"""
helpers = r'''    private static final float[] SRGB_TO_REC2020 = {
            0.627404f, 0.329283f, 0.043313f,
            0.069098f, 0.919555f, 0.011348f,
            0.016391f, 0.088029f, 0.895580f
    };

    private ColorSpaceTransform getTargetColorTransform() {
        assertCameraThread();
        if (lastAutoColorCorrectionTransform == null || cameraColorSpace != 3) {
            return lastAutoColorCorrectionTransform;
        }

        float[] source = new float[9];
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                android.util.Rational value = lastAutoColorCorrectionTransform.getElement(column, row);
                source[row * 3 + column] =
                        (float) value.getNumerator() / Math.max(1, value.getDenominator());
            }
        }

        float[] target = new float[9];
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                float value = 0;
                for (int k = 0; k < 3; ++k) {
                    value += SRGB_TO_REC2020[row * 3 + k] * source[k * 3 + column];
                }
                target[row * 3 + column] = value;
            }
        }

        int[] elements = new int[18];
        final int denominator = 1000000;
        for (int i = 0; i < 9; ++i) {
            elements[i * 2] = Math.round(target[i] * denominator);
            elements[i * 2 + 1] = denominator;
        }
        return new ColorSpaceTransform(elements);
    }

    private static String getActivePhysicalCameraId(TotalCaptureResult result) {
        if (Build.VERSION.SDK_INT < 29) {
            return null;
        }
        return result.get(TotalCaptureResult.LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID);
    }

    private boolean canApplyManualColorCorrection() {
        assertCameraThread();

        int[] capabilities = cameraCharacteristics.get(
                CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
        int[] awbModes = cameraCharacteristics.get(
                CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES);
        int[] correctionModes = cameraCharacteristics.get(
                CameraCharacteristics.COLOR_CORRECTION_AVAILABLE_MODES);

        boolean manualPostProcessing = contains(capabilities,
                CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING);
        boolean awbOff = contains(awbModes, CaptureRequest.CONTROL_AWB_MODE_OFF);
        boolean transformMatrix = Build.VERSION.SDK_INT < 36
                || contains(correctionModes, CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX);

        return manualPostProcessing && awbOff && transformMatrix;
    }

    private static boolean isAutoColorResultReady(TotalCaptureResult result) {
        Integer awbState = result.get(TotalCaptureResult.CONTROL_AWB_STATE);
        return awbState == null
                || awbState == TotalCaptureResult.CONTROL_AWB_STATE_CONVERGED
                || awbState == TotalCaptureResult.CONTROL_AWB_STATE_LOCKED;
    }

    private boolean hasToneMapMode(int mode) {
        int[] modes = cameraCharacteristics.get(CameraCharacteristics.TONEMAP_AVAILABLE_TONE_MAP_MODES);
        return contains(modes, mode);
    }

    private void applyGamma() {
        assertCameraThread();

        if (cameraGamma == 0) {
            requestBuilder.set(CaptureRequest.TONEMAP_MODE, CaptureRequest.TONEMAP_MODE_FAST);
            requestBuilder.set(CaptureRequest.TONEMAP_GAMMA, null);
            requestBuilder.set(CaptureRequest.TONEMAP_CURVE, null);
            requestBuilder.set(CaptureRequest.TONEMAP_PRESET_CURVE, null);
            return;
        }

        if (cameraTenBit && !cameraTenBitTo8Bit
                && (cameraGamma == 5 || cameraGamma == 6 || cameraGamma == 7)) {
            requestBuilder.set(CaptureRequest.TONEMAP_MODE, CaptureRequest.TONEMAP_MODE_FAST);
            requestBuilder.set(CaptureRequest.TONEMAP_GAMMA, null);
            requestBuilder.set(CaptureRequest.TONEMAP_CURVE, null);
            requestBuilder.set(CaptureRequest.TONEMAP_PRESET_CURVE, null);
            return;
        }

        if (cameraGamma == 1 || cameraGamma == 2) {
            if (!hasToneMapMode(CaptureRequest.TONEMAP_MODE_GAMMA_VALUE)) {
                throw new IllegalArgumentException("Camera does not support gamma-value tone mapping");
            }
            requestBuilder.set(CaptureRequest.TONEMAP_MODE, CaptureRequest.TONEMAP_MODE_GAMMA_VALUE);
            requestBuilder.set(CaptureRequest.TONEMAP_GAMMA, cameraGamma == 1 ? 2.2f : 2.4f);
            requestBuilder.set(CaptureRequest.TONEMAP_CURVE, null);
            requestBuilder.set(CaptureRequest.TONEMAP_PRESET_CURVE, null);
            return;
        }

        if (cameraGamma == 3 || cameraGamma == 4 || cameraGamma == 5) {
            int[] capabilities = cameraCharacteristics.get(
                    CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES);
            boolean manualPostProcessing = contains(capabilities,
                    CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING);
            if (!manualPostProcessing || !hasToneMapMode(CaptureRequest.TONEMAP_MODE_CONTRAST_CURVE)) {
                throw new IllegalArgumentException("Camera does not support custom tone mapping");
            }

            int maxPoints = 16;
            Integer advertised = cameraCharacteristics.get(
                    CameraCharacteristics.TONEMAP_MAX_CURVE_POINTS);
            if (advertised != null) {
                maxPoints = advertised;
            }

            int points = Math.max(2, Math.min(64, maxPoints));
            float[] curve = new float[points * 2];
            for (int i = 0; i < points; ++i) {
                float x = (float) i / (points - 1);
                float y;
                if (cameraGamma == 4) {
                    y = (float) Math.pow(x, 1.0 / 1.961);
                } else if (cameraGamma == 5) {
                    if (x <= 1.0f / 12.0f) {
                        y = (float) Math.sqrt(3.0 * x);
                    } else {
                        final double a = 0.17883277;
                        final double b = 0.28466892;
                        final double c = 0.55991073;
                        y = (float) (a * Math.log(12.0 * x - b) + c);
                    }
                } else {
                    if (x < 0.018f) {
                        y = 4.5f * x;
                    } else {
                        y = 1.099f * (float) Math.pow(x, 0.45) - 0.099f;
                    }
                }
                curve[i * 2] = x;
                curve[i * 2 + 1] = Math.max(0.0f, Math.min(1.0f, y));
            }

            TonemapCurve tonemap = new TonemapCurve(curve, curve, curve);
            requestBuilder.set(CaptureRequest.TONEMAP_MODE, CaptureRequest.TONEMAP_MODE_CONTRAST_CURVE);
            requestBuilder.set(CaptureRequest.TONEMAP_GAMMA, null);
            requestBuilder.set(CaptureRequest.TONEMAP_PRESET_CURVE, null);
            requestBuilder.set(CaptureRequest.TONEMAP_CURVE, tonemap);
            return;
        }

        if (cameraGamma == 8) {
            if (!hasToneMapMode(CaptureRequest.TONEMAP_MODE_PRESET_CURVE)) {
                throw new IllegalArgumentException("Camera does not support preset sRGB tone mapping");
            }
            requestBuilder.set(CaptureRequest.TONEMAP_MODE,
                    CaptureRequest.TONEMAP_MODE_PRESET_CURVE);
            requestBuilder.set(CaptureRequest.TONEMAP_PRESET_CURVE,
                    CaptureRequest.TONEMAP_PRESET_CURVE_SRGB);
            requestBuilder.set(CaptureRequest.TONEMAP_GAMMA, null);
            requestBuilder.set(CaptureRequest.TONEMAP_CURVE, null);
            return;
        }

        throw new IllegalArgumentException("Unknown camera gamma mode: " + cameraGamma);
    }


'''
if marker not in s:
    raise SystemExit("CameraCapture helper marker missing")
s=s.replace(marker,helpers+marker,1)
p.write_text(s,encoding="utf-8")


patch_generated("server/src/main/java/com/genymobile/scrcpy/video/CameraControlServer.java", [
    ("private static final int SETTINGS_SIZE = 21;", "private static final int SETTINGS_SIZE = 26;"),
    (
        """            int wbKelvin = in.readInt();

            capture.setCameraSettings(zoom, torch, iso, shutterUs, focusDistance, wbKelvin);
""",
        """            int wbKelvin = in.readInt();
            boolean wbLock = in.readUnsignedByte() != 0;
            int colorSpace = in.readUnsignedByte();
            int gamma = in.readUnsignedByte();
            boolean tenBit = in.readUnsignedByte() != 0;
            int dynamicRange = in.readUnsignedByte();

            capture.setCameraSettings(zoom, torch, iso, shutterUs, focusDistance, wbKelvin, wbLock,
                    colorSpace, gamma, tenBit, dynamicRange);
""",
    ),
])

# Apply the real Camera2 AWB lock. The UI only enables it in Auto mode.
patch_generated("server/src/main/java/com/genymobile/scrcpy/video/CameraCapture.java", [
    (
        """            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                    CaptureRequest.CONTROL_AWB_MODE_AUTO);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_GAINS, null);
""",
        """            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                    CaptureRequest.CONTROL_AWB_MODE_AUTO);
            requestBuilder.set(CaptureRequest.CONTROL_AWB_LOCK, cameraWbLock);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_GAINS, null);
""",
    ),
    (
        """                requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                        CaptureRequest.CONTROL_AWB_MODE_OFF);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE,
""",
        """                requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                        CaptureRequest.CONTROL_AWB_MODE_OFF);
                requestBuilder.set(CaptureRequest.CONTROL_AWB_LOCK, false);
                requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE,
""",
    ),
    (
        """            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                    CaptureRequest.CONTROL_AWB_MODE_OFF);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE,
                    CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_TRANSFORM,
""",
        """            requestBuilder.set(CaptureRequest.CONTROL_AWB_MODE,
                    CaptureRequest.CONTROL_AWB_MODE_OFF);
            requestBuilder.set(CaptureRequest.CONTROL_AWB_LOCK, false);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_MODE,
                    CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX);
            requestBuilder.set(CaptureRequest.COLOR_CORRECTION_TRANSFORM,
""",
    ),
])

print("scrcpy camera patch applied")
