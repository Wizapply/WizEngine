#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

typedef struct _GstElement GstElement;
typedef struct _GMainLoop GMainLoop;

// Streams the rendered frames to a browser over WebRTC using GStreamer's
// webrtcbin - no external media server. Signaling is WHEP-style: the browser
// POSTs an SDP offer and handleOffer() returns the SDP answer. Frames are fed
// with pushFrame(). Requires the GStreamer webrtc/nice plugins.
//
// The WebRTC pipeline is rebuilt on every offer, so reloading the browser (or a
// new viewer) starts a fresh session. One viewer at a time.
// Video codec used for the WebRTC stream.
// - VP8:  CPU encode; the safe baseline, works everywhere.
// - VP9:  CPU encode; ~half the bitrate of VP8 for the same quality, but
//         noticeably more CPU per frame.
// - H264: hardware encode when available (AMD AMF -> Media Foundation ->
//         software fallback). Frees the CPU almost entirely; bitrate
//         efficiency sits between VP8 and VP9.
enum class VideoCodec { VP8, VP9, H264 };

class WebRtcStreamer {
public:
    WebRtcStreamer(int width, int height, int fps,
                   VideoCodec codec = VideoCodec::VP8, int bitrateBps = 6000000);

    const char* codecName() const;
    int bitrateBps() const { return bitrate_; }
    ~WebRtcStreamer();

    WebRtcStreamer(const WebRtcStreamer&) = delete;
    WebRtcStreamer& operator=(const WebRtcStreamer&) = delete;

    void pushFrame(const uint8_t* rgba, std::size_t size);

    // Given the browser's SDP offer, returns the SDP answer (blocking, with a
    // timeout). Empty string on failure.
    std::string handleOffer(const std::string& offerSdp);

    // Tear down the current WebRTC session (called when the viewer leaves), so
    // the next viewer starts from a clean pipeline.
    void stopSession();

private:
    // Create a fresh appsrc->encoder->webrtcbin pipeline. payloadType is the
    // RTP payload number the browser assigned to our codec in its offer: as
    // the answering side we must send with exactly that PT, or the browser
    // silently discards every packet (black picture, no errors anywhere).
    void buildPipeline(int payloadType, const std::string& h264ProfileLevelId);
    void teardownPipeline();  // stop and free the current pipeline

    int width_;
    int height_;
    int fps_;
    VideoCodec codec_;
    int bitrate_;
    GstElement* pipeline_ = nullptr;
    GstElement* appsrc_ = nullptr;
    // Keyframe warm-up: ask for an IDR every second for the first 5 seconds of
    // a session, so a keyframe lost during the handshake cannot leave the
    // browser stuck on a black frame.
    static constexpr unsigned kKeyframeEveryFrames = 60;
    static constexpr unsigned kKeyframeWarmupFrames = 300;
    unsigned framesSinceStart_ = 0;
    GstElement* webrtc_ = nullptr;
    GMainLoop* loop_ = nullptr;
    unsigned busWatchId_ = 0;  // must be removed before the pipeline is freed
    std::thread loopThread_;

    std::mutex pipelineMutex_;  // guards pipeline_/appsrc_/webrtc_ vs pushFrame
    std::mutex handleMutex_;    // serializes offers (one negotiation at a time)
};
