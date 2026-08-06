#include "WebRtcStreamer.h"

#include "Log.h"

#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <string>
#include <vector>
#include <stdexcept>

namespace {

// Per-offer state carried through the async webrtcbin negotiation callbacks.
struct OfferCtx {
    GstElement* webrtc = nullptr;
    std::promise<std::string> answer;
    bool done = false;
};

std::string localDescriptionSdp(GstElement* webrtc) {
    GstWebRTCSessionDescription* desc = nullptr;
    g_object_get(webrtc, "local-description", &desc, nullptr);
    if (!desc) return {};
    gchar* text = gst_sdp_message_as_text(desc->sdp);
    std::string out = text ? text : "";
    g_free(text);
    gst_webrtc_session_description_free(desc);
    return out;
}

void finish(OfferCtx* ctx) {
    if (ctx->done) return;
    ctx->done = true;
    ctx->answer.set_value(localDescriptionSdp(ctx->webrtc));
}

void onIceGatheringNotify(GstElement* webrtc, GParamSpec*, gpointer user_data) {
    auto* ctx = static_cast<OfferCtx*>(user_data);
    GstWebRTCICEGatheringState state = GST_WEBRTC_ICE_GATHERING_STATE_NEW;
    g_object_get(webrtc, "ice-gathering-state", &state, nullptr);
    if (state == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) finish(ctx);
}

void onAnswerCreated(GstPromise* promise, gpointer user_data) {
    auto* ctx = static_cast<OfferCtx*>(user_data);
    const GstStructure* reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription* answer = nullptr;
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer,
                      nullptr);
    gst_promise_unref(promise);
    if (!answer) {
        finish(ctx);
        return;
    }

    GstPromise* p = gst_promise_new();
    g_signal_emit_by_name(ctx->webrtc, "set-local-description", answer, p);
    gst_promise_interrupt(p);
    gst_promise_unref(p);
    gst_webrtc_session_description_free(answer);

    // Non-trickle ICE: return the answer once gathering completes.
    g_signal_connect(ctx->webrtc, "notify::ice-gathering-state",
                     G_CALLBACK(onIceGatheringNotify), ctx);
    GstWebRTCICEGatheringState state = GST_WEBRTC_ICE_GATHERING_STATE_NEW;
    g_object_get(ctx->webrtc, "ice-gathering-state", &state, nullptr);
    if (state == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) finish(ctx);
}

void onRemoteSet(GstPromise* promise, gpointer user_data) {
    auto* ctx = static_cast<OfferCtx*>(user_data);
    gst_promise_unref(promise);
    GstPromise* p = gst_promise_new_with_change_func(onAnswerCreated, ctx, nullptr);
    g_signal_emit_by_name(ctx->webrtc, "create-answer", nullptr, p);
}

// Payload choice extracted from the browser's offer for a given codec name.
struct PayloadChoice {
    int pt = -1;
    std::string profileLevelId;  // H264 only: echoed back in our answer caps
};

std::string fmtpLine(const std::string& sdp, int pt) {
    const std::string key = "a=fmtp:" + std::to_string(pt) + " ";
    const size_t at = sdp.find(key);
    if (at == std::string::npos) return {};
    size_t eol = sdp.find('\n', at);
    std::string line = sdp.substr(at + key.size(),
                                  (eol == std::string::npos ? sdp.size() : eol) -
                                      at - key.size());
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

// The RTP payload number the browser's offer assigned to `codec` ("VP8",
// "VP9" or "H264"), picking the variant we can actually produce:
// - VP9: profile-id 0 (browsers offer profile 0 and 2 under different PTs)
// - H264: packetization-mode=1 (rtph264pay's default framing)
// Returns pt = -1 if the codec is not in the offer at all.
PayloadChoice payloadFromOffer(const std::string& sdp, const std::string& codec) {
    const std::string needle = " " + codec + "/90000";
    std::vector<int> pts;

    size_t pos = 0;
    while (pos < sdp.size()) {
        size_t eol = sdp.find('\n', pos);
        if (eol == std::string::npos) eol = sdp.size();
        std::string line = sdp.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = eol + 1;

        // a=rtpmap:98 VP9/90000
        if (line.rfind("a=rtpmap:", 0) == 0 && line.find(needle) != std::string::npos) {
            pts.push_back(std::atoi(line.c_str() + 9));
        }
    }

    PayloadChoice fallback;
    if (!pts.empty()) fallback.pt = pts.front();

    for (int pt : pts) {
        const std::string fmtp = fmtpLine(sdp, pt);
        if (codec == "VP9") {
            if (fmtp.find("profile-id=") != std::string::npos &&
                fmtp.find("profile-id=0") == std::string::npos) {
                continue;  // profile 1/2/3: not what we encode
            }
            return {pt, {}};
        }
        if (codec == "H264") {
            if (fmtp.find("packetization-mode=1") == std::string::npos) continue;
            PayloadChoice c;
            c.pt = pt;
            const size_t at = fmtp.find("profile-level-id=");
            if (at != std::string::npos) {
                size_t end = fmtp.find(';', at);
                c.profileLevelId = fmtp.substr(
                    at + 17, (end == std::string::npos ? fmtp.size() : end) - at - 17);
            }
            return c;
        }
        return {pt, {}};  // VP8: first entry is fine
    }
    return fallback;
}

}  // namespace

const char* WebRtcStreamer::codecName() const {
    switch (codec_) {
        case VideoCodec::VP9: return "VP9";
        case VideoCodec::H264: return "H264";
        default: return "VP8";
    }
}

WebRtcStreamer::WebRtcStreamer(int width, int height, int fps, VideoCodec codec,
                               int bitrateBps)
    : width_(width),
      height_(height),
      fps_(fps),
      codec_(codec),
      bitrate_(bitrateBps) {
    gst_init(nullptr, nullptr);
    // GLib's default print handlers write raw lines to stdout/stderr; route
    // them through the logger so GStreamer's own output keeps the format.
    g_set_print_handler([](const gchar* text) { LOGI("gst", "%s", text); });
    g_set_printerr_handler([](const gchar* text) { LOGW("gst", "%s", text); });
    // The media pipeline is built fresh on each offer (see handleOffer). A GLib
    // main loop runs for webrtcbin/libnice callbacks.
    loop_ = g_main_loop_new(nullptr, FALSE);
    loopThread_ = std::thread([this] { g_main_loop_run(loop_); });
}

WebRtcStreamer::~WebRtcStreamer() {
    {
        std::lock_guard<std::mutex> lk(pipelineMutex_);
        teardownPipeline();
    }
    if (loop_) g_main_loop_quit(loop_);
    if (loopThread_.joinable()) loopThread_.join();
    if (loop_) g_main_loop_unref(loop_);
}

// First H.264 encoder element installed on this machine, hardware first.
// Returns nullptr if none exist.
static const char* pickH264Encoder() {
    static const char* const candidates[] = {
        "amfh264enc",   // AMD hardware (AMF)
        "mfh264enc",    // Windows Media Foundation (may also be hardware)
        "x264enc",      // software fallback
        "openh264enc",  // software fallback
    };
    for (const char* name : candidates) {
        if (GstElementFactory* f = gst_element_factory_find(name)) {
            gst_object_unref(f);
            return name;
        }
    }
    return nullptr;
}

void WebRtcStreamer::buildPipeline(int payloadType,
                                   const std::string& h264ProfileLevelId) {
    const int pt = (payloadType > 0) ? payloadType : 96;
    // No STUN server: for localhost / same-LAN use, host ICE candidates are
    // enough. (For NAT traversal across networks, add a reachable stun-server.)
    //
    // target-bitrate drives quality - the encoders default to only 256 kbps,
    // which looks blocky at 720p. deadline=1 (realtime) and lag-in-frames=0
    // keep latency low; cpu-used trades quality for speed, and VP9 needs a
    // higher value than VP8 to keep up at 60 fps.
    const bool vp9 = (codec_ == VideoCodec::VP9);
    const bool h264 = (codec_ == VideoCodec::H264);

    std::string enc264;
    if (h264) {
        const char* encName = pickH264Encoder();
        if (!encName) {
            LOGE("webrtc",
                 "no H.264 encoder found (amfh264enc/mfh264enc/x264enc/"
                 "openh264enc) - install the matching GStreamer plugin");
            return;
        }
        LOGI("webrtc", "h264 encoder: %s", encName);
        const int kbps = bitrate_ / 1000;
        std::string e = encName;
        if (e == "amfh264enc") {
            // AMD AMF: usage=ultra-low-latency keeps the encoder queue at one
            // frame; bitrate is in kbit/s; a 1 s GOP for recovery after loss.
            enc264 = "amfh264enc usage=ultra-low-latency rate-control=cbr "
                     "bitrate=" + std::to_string(kbps) + " gop-size=60";
        } else if (e == "mfh264enc") {
            enc264 = "mfh264enc low-latency=true bitrate=" + std::to_string(kbps) +
                     " gop-size=60";
        } else if (e == "x264enc") {
            enc264 = "x264enc tune=zerolatency speed-preset=ultrafast "
                     "bitrate=" + std::to_string(kbps) + " key-int-max=60";
        } else {  // openh264enc (bitrate is in bit/s)
            enc264 = "openh264enc bitrate=" + std::to_string(bitrate_) +
                     " gop-size=60 complexity=low";
        }
        // h264parse + config-interval=-1: re-send SPS/PPS with every keyframe.
        // Browsers cannot decode without in-band parameter sets - leaving this
        // out is the classic "negotiates fine, stays black" H.264 mistake.
        //
        // NOTE the RTP caps deliberately do NOT pin profile-level-id: that
        // string must match what the encoder actually emits, and hardware
        // encoders typically produce Main/High regardless of what the browser
        // listed for that payload (Chrome's 42001f = Baseline). Forcing the
        // browser's value onto caps the encoder cannot satisfy fails
        // negotiation inside the pipeline ("streaming stopped, not-negotiated")
        // before a single frame is sent. Browsers decode whatever profile the
        // in-band SPS announces, so leaving it off is both safe and correct.
        enc264 += " ! h264parse config-interval=-1 ! "
                  "rtph264pay pt=" + std::to_string(pt) +
                  " config-interval=-1 aggregate-mode=zero-latency ! "
                  "application/x-rtp,media=video,encoding-name=H264,payload=" +
                  std::to_string(pt) +
                  ",clock-rate=90000,packetization-mode=(string)1";
        (void)h264ProfileLevelId;
    }

    const std::string enc =
        // VP9. Kept deliberately close to the VP8 line that is known to work:
        // extra caps filters on the encoder output turned out to be a good way
        // to get a silently black picture. picture-id-mode is the one addition
        // that matters - browsers expect a picture ID in the VP9 payload.
        // profile-id=0 (8-bit 4:2:0) has to appear in the SDP: browsers assume
        // it, but rtpvp9pay does not advertise it, and a VP9 stream without it
        // negotiates fine and then decodes to nothing - a silent black picture.
        h264 ? enc264
        : vp9 ? ("vp9enc deadline=1 cpu-used=8 target-bitrate=" +
               std::to_string(bitrate_) +
               " end-usage=cbr keyframe-max-dist=60 lag-in-frames=0 ! "
               "rtpvp9pay pt=" + std::to_string(pt) +
               " picture-id-mode=15-bit ! "
               "application/x-rtp,media=video,encoding-name=VP9,payload=" +
               std::to_string(pt) +
               ",clock-rate=90000,profile-id=(string)0")
            : ("vp8enc deadline=1 cpu-used=4 target-bitrate=" +
               std::to_string(bitrate_) +
               " end-usage=cbr keyframe-max-dist=60 lag-in-frames=0 ! "
               "rtpvp8pay pt=" + std::to_string(pt) +
               " ! "
               "application/x-rtp,media=video,encoding-name=VP8,payload=" +
               std::to_string(pt) + ",clock-rate=90000");

    const std::string desc =
        "appsrc name=src is-live=true format=time do-timestamp=true ! "
        // Force 8-bit 4:2:0 before the encoder: RGBA can otherwise be taken to
        // a format the browser will not decode. Hardware H.264 encoders take
        // NV12 (same 4:2:0 data, different layout); the VP encoders take I420.
        "videoconvert ! video/x-raw,format=" +
        std::string(h264 ? "NV12" : "I420") + " ! " + enc +
        " ! webrtcbin name=wb bundle-policy=max-bundle";

    LOGI("webrtc", "pipeline: %s", desc.c_str());

    GError* err = nullptr;
    pipeline_ = gst_parse_launch(desc.c_str(), &err);
    if (!pipeline_) {
        LOGE("webrtc",
             "pipeline failed (is the GStreamer webrtc plugin, and the %s "
             "encoder, installed?): %s",
             codecName(), err ? err->message : "unknown");
        if (err) g_error_free(err);
        return;
    }
    if (err) g_error_free(err);

    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
    webrtc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "wb");

    const std::string caps =
        "video/x-raw,format=RGBA,width=" + std::to_string(width_) +
        ",height=" + std::to_string(height_) +
        ",framerate=" + std::to_string(fps_) + "/1";
    GstCaps* c = gst_caps_from_string(caps.c_str());
    g_object_set(appsrc_, "caps", c, nullptr);
    gst_caps_unref(c);

    // Surface encoder/negotiation problems instead of failing silently to a
    // black picture.
    if (GstBus* bus = gst_element_get_bus(pipeline_)) {
        busWatchId_ = gst_bus_add_watch(
            bus,
            [](GstBus*, GstMessage* msg, gpointer) -> gboolean {
                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                    GError* e = nullptr;
                    gchar* dbg = nullptr;
                    gst_message_parse_error(msg, &e, &dbg);
                    LOGE("gst", "error from %s: %s (%s)",
                         GST_OBJECT_NAME(msg->src), e->message,
                         dbg ? dbg : "");
                    g_error_free(e);
                    g_free(dbg);
                } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
                    GError* e = nullptr;
                    gchar* dbg = nullptr;
                    gst_message_parse_warning(msg, &e, &dbg);
                    LOGW("gst", "warning from %s: %s (%s)",
                         GST_OBJECT_NAME(msg->src), e->message,
                         dbg ? dbg : "");
                    g_error_free(e);
                    g_free(dbg);
                }
                return TRUE;
            },
            nullptr);
        gst_object_unref(bus);
    }

    framesSinceStart_ = 0;
    const GstStateChangeReturn ret =
        gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOGE("webrtc", "pipeline failed to start");
    }
}

void WebRtcStreamer::teardownPipeline() {
    if (!pipeline_) return;
    // Remove the bus watch first: it holds a reference to this pipeline and
    // fires from the GLib loop thread, so leaving it in place while the
    // pipeline is freed crashes on the next reconnect.
    if (busWatchId_ != 0) {
        g_source_remove(busWatchId_);
        busWatchId_ = 0;
    }
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (appsrc_) {
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
    }
    if (webrtc_) {
        gst_object_unref(webrtc_);
        webrtc_ = nullptr;
    }
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
}

void WebRtcStreamer::pushFrame(const uint8_t* rgba, std::size_t size) {
    std::lock_guard<std::mutex> lk(pipelineMutex_);
    if (!appsrc_) return;  // no active session yet
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    gst_buffer_fill(buffer, 0, rgba, size);
    gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);

    // A session's first keyframe is produced before the DTLS/ICE handshake
    // finishes, so the browser misses it and shows black until the next one.
    // Request extra keyframes during the first seconds of a session; after
    // that the encoder's own GOP is enough.
    ++framesSinceStart_;
    if (framesSinceStart_ <= kKeyframeWarmupFrames &&
        framesSinceStart_ % kKeyframeEveryFrames == 0) {
        gst_element_send_event(
            pipeline_,
            gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE,
                                                        TRUE, 0));
    }
}

void WebRtcStreamer::stopSession() {
    std::lock_guard<std::mutex> plk(pipelineMutex_);
    teardownPipeline();
}

std::string WebRtcStreamer::handleOffer(const std::string& offerSdp) {
    // One negotiation at a time; rebuild the pipeline so each connection (and
    // browser reload) gets a fresh WebRTC session.
    std::lock_guard<std::mutex> hlk(handleMutex_);

    // Answering side: send with the payload number the browser's offer chose
    // for our codec (and, for H264, its exact profile-level-id). Chrome happens
    // to offer VP8 at 96 but VP9/H264 under other numbers - answering with a
    // mismatched PT means every packet is silently dropped: black picture.
    const PayloadChoice pc = payloadFromOffer(offerSdp, codecName());
    LOGI("webrtc", "browser offered %s at payload %d%s%s", codecName(), pc.pt,
         pc.profileLevelId.empty() ? "" : ", profile-level-id=",
         pc.profileLevelId.c_str());

    GstElement* webrtc = nullptr;
    {
        std::lock_guard<std::mutex> plk(pipelineMutex_);
        teardownPipeline();
        buildPipeline(pc.pt, pc.profileLevelId);
        webrtc = webrtc_;
    }
    if (!webrtc) return {};

    GstSDPMessage* sdp = nullptr;
    gst_sdp_message_new(&sdp);
    if (gst_sdp_message_parse_buffer(
            reinterpret_cast<const guint8*>(offerSdp.data()),
            static_cast<guint>(offerSdp.size()), sdp) != GST_SDP_OK) {
        gst_sdp_message_free(sdp);
        return {};
    }
    GstWebRTCSessionDescription* offer =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);

    OfferCtx ctx;
    ctx.webrtc = webrtc;
    auto fut = ctx.answer.get_future();

    GstPromise* p = gst_promise_new_with_change_func(onRemoteSet, &ctx, nullptr);
    g_signal_emit_by_name(webrtc, "set-remote-description", offer, p);
    gst_webrtc_session_description_free(offer);

    if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        return {};
    }
    return fut.get();
}
