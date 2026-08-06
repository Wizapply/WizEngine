#include "VideoStreamer.h"
#include "Log.h"

#include <gst/app/gstappsrc.h>

#include <stdexcept>
#include <string>

VideoStreamer::VideoStreamer(OutputMode mode, int width, int height, int fps,
                             const std::string& host, int port)
    : width_(width), height_(height), fps_(fps) {
    gst_init(nullptr, nullptr);
    // See WebRtcStreamer: GLib output goes through the logger.
    g_set_print_handler([](const gchar* text) { LOGI("gst", "%s", text); });
    g_set_printerr_handler([](const gchar* text) { LOGW("gst", "%s", text); });

    // "web" / None mode: no GStreamer output. Frames reach the browser via the
    // browser via WebRtcStreamer instead, so leave the GStreamer pipeline null.
    if (mode == OutputMode::None) return;

    // Common source. Note: no vertical flip - Filament's Vulkan backend returns
    // readPixels data top-down already. (If you switch to the OpenGL backend,
    // which returns bottom-up, re-add "videoflip method=vertical-flip ! " here.)
    const std::string src =
        "appsrc name=src is-live=true format=time do-timestamp=true ! "
        "videoconvert ! ";

    std::string desc;
    if (mode == OutputMode::Window) {
        // Local preview window on this machine - no encoding needed.
        desc = src + "autovideosink sync=false";
    } else if (mode == OutputMode::Rtsp) {
        // Encode and PUBLISH to an RTSP server (e.g. MediaMTX). MediaMTX then
        // re-serves it as WebRTC/HLS for browsers. `host` carries the full RTSP
        // URL here (e.g. rtsp://127.0.0.1:8554/wiz).
        desc = src +
               "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=" +
               std::to_string(fps_) +
               " ! h264parse ! rtspclientsink latency=0 location=" + host;
    } else {
        // Encode to H.264 and send as RTP/UDP. key-int-max = fps -> ~1 keyframe/s.
        desc = src +
               "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=" +
               std::to_string(fps_) +
               " ! rtph264pay config-interval=1 pt=96 ! "
               "udpsink host=" + host + " port=" + std::to_string(port);
    }

    GError* err = nullptr;
    pipeline_ = gst_parse_launch(desc.c_str(), &err);
    if (!pipeline_) {
        const std::string msg = err ? err->message : "gst_parse_launch failed";
        if (err) g_error_free(err);
        throw std::runtime_error(msg);
    }
    if (err) g_error_free(err);

    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");

    const std::string caps =
        "video/x-raw,format=RGBA,width=" + std::to_string(width_) +
        ",height=" + std::to_string(height_) +
        ",framerate=" + std::to_string(fps_) + "/1";
    GstCaps* c = gst_caps_from_string(caps.c_str());
    g_object_set(appsrc_, "caps", c, nullptr);
    gst_caps_unref(c);

    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
}

VideoStreamer::~VideoStreamer() {
    if (appsrc_) {
        gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
        gst_object_unref(appsrc_);
    }
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
    }
}

void VideoStreamer::pushFrame(const uint8_t* rgba, size_t size) {
    if (!appsrc_) return;  // None mode: nothing to push to.
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    gst_buffer_fill(buffer, 0, rgba, size);
    // push_buffer takes ownership of the buffer.
    gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
}
