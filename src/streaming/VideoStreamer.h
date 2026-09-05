#pragma once

#include <gst/gst.h>

#include <cstdint>
#include <string>

// How rendered frames are presented.
enum class OutputMode {
    None,    // no GStreamer output; browser views via WebRTC (WebRtcStreamer)
    Window,  // show locally in a GStreamer preview window (no encoding)
    Stream,  // encode to H.264 and send as RTP/UDP to host:port
    Rtsp,    // encode to H.264 and publish to an RTSP server (e.g. MediaMTX)
};

// Wraps a GStreamer pipeline. Feed RGBA frames with pushFrame().
//   Window: appsrc -> videoconvert -> videoflip -> autovideosink
//   Stream: appsrc -> videoconvert -> videoflip -> x264enc -> rtph264pay -> udpsink
class VideoStreamer {
public:
    VideoStreamer(OutputMode mode, int width, int height, int fps,
                  const std::string& host, int port);
    ~VideoStreamer();

    VideoStreamer(const VideoStreamer&) = delete;
    VideoStreamer& operator=(const VideoStreamer&) = delete;

    void pushFrame(const uint8_t* rgba, size_t size);

private:
    GstElement* pipeline_ = nullptr;
    GstElement* appsrc_ = nullptr;
    int width_;
    int height_;
    int fps_;
};
