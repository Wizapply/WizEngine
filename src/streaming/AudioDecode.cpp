#include "streaming/AudioDecode.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace wizengine {
namespace streaming {

bool decodeAudioFileGst(const std::string& path, audio::AudioClip& out, std::string& error) {
    gst_init(nullptr, nullptr);
    GError* err = nullptr;
    // location は g_object_set で渡す（パスの空白や記号を parse の構文に通さない）。
    GstElement* pipeline = gst_parse_launch(
        "filesrc name=fs ! decodebin ! audioconvert ! audioresample ! "
        "audio/x-raw,format=F32LE,channels=2,rate=48000,layout=interleaved ! "
        "appsink name=sink sync=false",
        &err);
    if (!pipeline) {
        error = std::string("decoder pipeline failed: ") + (err ? err->message : "unknown");
        if (err) g_error_free(err);
        return false;
    }
    if (err) g_error_free(err);
    GstElement* fs = gst_bin_get_by_name(GST_BIN(pipeline), "fs");
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    g_object_set(fs, "location", path.c_str(), nullptr);
    gst_object_unref(fs);

    std::vector<float> samples;
    bool ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    const std::size_t limit = audio::kMaxClipFrames * 2;
    while (ok) {
        // 5 秒何も来なければ諦める（壊れたファイルで読み込みスレッドを止めない）。
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND);
        if (!sample) break;  // EOS か時間切れ（下でエラーを確かめる）
        GstBuffer* buf = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
            const std::size_t n = map.size / sizeof(float);
            const std::size_t old = samples.size();
            const std::size_t take = std::min(n, limit > old ? limit - old : 0);
            samples.resize(old + take);
            std::memcpy(samples.data() + old, map.data, take * sizeof(float));
            gst_buffer_unmap(buf, &map);
        }
        gst_sample_unref(sample);
        if (samples.size() >= limit) break;
    }
    // 失敗の理由（デコーダが無い・壊れたファイル）はバスのエラーに出る。
    if (GstBus* bus = gst_element_get_bus(pipeline)) {
        if (GstMessage* msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
            GError* e = nullptr;
            gst_message_parse_error(msg, &e, nullptr);
            error = e ? e->message : "decode error";
            if (e) g_error_free(e);
            gst_message_unref(msg);
            ok = false;
        }
        gst_object_unref(bus);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    if (!ok) {
        if (error.empty()) error = "could not start the decoder";
        return false;
    }
    if (samples.empty()) {
        error = "no audio decoded (missing GStreamer plugin for this format?)";
        return false;
    }
    bool truncated = false;
    out = audio::makeClip(samples, 2, audio::kSampleRate, &truncated);
    if (truncated) error = "longer than 10 minutes - truncated";
    return true;
}

}  // namespace streaming
}  // namespace wizengine
