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
// - VP9:  CPU (software) encode; the fallback when no hardware exists.
// - H264: hardware encode when available (AMD AMF -> Media Foundation ->
//         software fallback). Frees the CPU almost entirely; every browser
//         and device decodes it - the compatibility baseline.
// - H265: hardware encode; better quality per bit than H264, but viewers are
//         Chrome 136+ / Safari 18+ only (Edge and Firefox never offer it).
//         Falls back to H264 at startup when no encoder exists.
// - AV1:  hardware encode (RDNA3+/RTX40+/Arc); best compression, royalty
//         free, Chrome/Edge/Firefox viewers. Needs rtpav1pay
//         (gst-plugins-rs); missing pieces fall back to H264 at startup.
enum class VideoCodec { VP9, H264, H265, AV1 };

class WebRtcStreamer {
public:
    WebRtcStreamer(int width, int height, int fps,
                   VideoCodec codec = VideoCodec::H264, int bitrateBps = 6000000);

    const char* codecName() const;
    int bitrateBps() const { return bitrate_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int fps() const { return fps_; }

    // Changes the stream format for the NEXT negotiation (the pipeline is
    // rebuilt on every offer, so a reconnect picks this up). Frames of the
    // old size that are still in flight are dropped, not pushed into a
    // mismatched pipeline. Any thread.
    void setStreamFormat(int width, int height, int fps, int bitrateBps);

    // Forces a specific H.264 encoder element instead of the automatic
    // hardware-first pick - set once at startup (from --encoder) before any
    // streamer negotiates. The point is GPU selection: GStreamer registers
    // one element per GPU (amfh264enc = primary adapter, amfh264device1enc =
    // the next one, nvh264device0enc, ...), so choosing the element chooses
    // the GPU. An element that is not installed logs a warning and falls
    // back to the automatic pick. Applies to the H264, H265 and AV1 pickers
    // (whichever codec is active); VP9 is software and unaffected.
    static void setEncoderOverride(const std::string& name);

    // Colour conversion (RGBA -> NV12) on the GPU via d3d11convert instead of
    // CPU videoconvert - saves roughly one core per camera at 1080p60. Only
    // taken when the encoder is a D3D11-capable hardware family (amf/mf/
    // nvd3d11) and the d3d11 elements exist; otherwise the CPU path is used
    // regardless of this setting. Set from main (kGpuConvert) before
    // the endpoints are created.
    static void setPreferGpuConvert(bool prefer);
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
