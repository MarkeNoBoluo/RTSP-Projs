#pragma once

struct PusherConfig {
    const char* rtspUrl = "rtsp://192.168.42.116:25544/live";

    // Screen capture: 0 = all monitors (virtual desktop), 1+ = specific monitor
    int screenIndex    = 0;
    int captureOffsetX = 0;   // gdigrab offset_x (set automatically when screenIndex > 0)
    int captureOffsetY = 0;   // gdigrab offset_y
    int captureWidth   = 2560;
    int captureHeight  = 1440;
    int captureFps     = 30;

    int outputWidth  = 1920;
    int outputHeight = 1080;
    // All bitrate/bufsize fields are in kbps (kilobits per second).
    // They are multiplied by 1000 before being passed to FFmpeg (which expects bps).
    // Default values form a CBR profile suitable for low-latency 1080p desktop streaming.
    int videoBitrate = 20000;
    int videoMaxrate = 8000;
    int videoBufsize = 20000;
    int gopSize      = 30;
    int crf          = 0;        // 0 = CRF disabled (use ABR); >0 = CRF mode

    bool enableAudio = true;
    const char* audioDeviceName = nullptr; // nullptr = SDL default capture device
    int audioDeviceIndex = -1;             // -1 = use audioDeviceName/default
    bool requireAudio = false;             // true when user explicitly selected audio
    int audioSampleRate = 48000;
    int audioChannels   = 2;
    int audioBitrate    = 128;   // kbps

    const char* rtspTransport = "tcp";

    // Hardware encoder selection ("off", "auto", "qsv", "vaapi", "nvenc", or codec name)
    const char* hwEncoder = "off";

    // VAAPI DRM device path (default: /dev/dri/renderD128)
    const char* drmDevice = "/dev/dri/renderD128";

    // Capture backend: "auto" (default), "gdigrab", "ddagrab"
    const char* captureMethod = "auto";
};
