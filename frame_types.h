#pragma once
#include <QtCore>
#include "../Hamamatsu_DCAMSDK4_v25056964/dcamsdk4/inc/dcamprop.h"
#include "../Hamamatsu_DCAMSDK4_v25056964/dcamsdk4/inc/dcamapi4.h"

struct ApplySettings {
    int width = 0;
    int height = 0;
    int binning = 1;
    int bits = 12;     // default to camera min (mono8 removed)
    int pixelType = DCAM_PIXELTYPE_MONO8; // default mono8 to maximize FPS
    bool enableSubarray = true;
    double exposure_s = 0.010; // default 10 ms
    int readoutSpeed = DCAMPROP_READOUTSPEED__FASTEST;
    bool bundleEnabled = false;
    int bundleCount = 0;
    bool binningIndependent = false;
    int binH = 1;
    int binV = 1;
};

struct FrameMeta {
    int width = 0;
    int height = 0;
    int bits = 0;
    double binning = 1.0;
    qint64 frameIndex = 0;
    qint64 delivered = 0;
    qint64 dropped = 0;
    double internalFps = 0.0;
    double readoutSpeed = 0.0;
};
