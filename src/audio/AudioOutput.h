#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/AudioClip.h"
#include "audio/AudioFrame.h"
#include "audio/AudioMixer.h"

// サーバーで音を作って映像と一緒に送る係（カメラ = ページごとに 1 つのミキサー）。
//
//   RENDER スレッド: submit(cam, frame)      … 描いたフレームと同じ瞬間の状態
//                    setVideoLatency(cam, s)  … その絵を描いてからエンコーダへ押し込む
//                                              までの秒数（Renderer::readbackLatency）
//   音声のスレッド:  10 ms（480 フレーム）ごとに、最新の状態でミキサーを回して
//                    sink(cam, pcm, frames, latency) へ渡す（WebRtcStreamer::pushAudio）
//
// **映像との同期**: 映像の PTS は「絵をエンコーダへ押し込んだ時刻」（appsrc の
// do-timestamp）= 描いた時刻 + 読み戻しの遅れ。音は「いまの状態」で作るので、
// 同じ時計（パイプラインの時計）で「いま + 読み戻しの遅れ」を PTS にすれば、同じ
// 瞬間の絵と音が同じ時刻になる（PTS の付け方は sink の側 = WebRtcStreamer）。
// ブラウザはこの 2 本を同じ MediaStream（msid）として受け、RTCP の送信者報告で
// 揃えて出す。
//
// 実時間ちょうどの量を作る: 開始からの経過 × 48000 に追いつくまでブロックを作る
// （スレッドの寝坊で量がぶれない）。0.2 秒より遅れたら追いつくのを諦めて飛ばす
// （sink 側が PTS を付け直す）。視聴者のいないカメラ（1 秒 submit が無い）は回さない。
namespace wizengine {
namespace audio {

class AudioOutput {
public:
    // pcm: 48 kHz ステレオ int16 の交互配置、latency: 映像の遅れ（s）。
    using Sink = std::function<void(const std::int16_t* pcm, std::size_t frames, double latency)>;

    static constexpr std::size_t kBlockFrames = 480;  // 10 ms

    AudioOutput(std::size_t cameras, AudioClipCache& clips);
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // 起動の前に（main）。
    void setSink(std::size_t cam, Sink sink);
    void start();
    void stop();

    // RENDER スレッド。
    void submit(std::size_t cam, AudioFrame frame);
    void setVideoLatency(std::size_t cam, double seconds);

private:
    struct Slot {
        std::unique_ptr<AudioMixer> mixer;
        Sink sink;
        std::mutex mutex;          // frame / hasFrame / lastSubmit
        AudioFrame frame;
        bool hasFrame = false;
        std::uint64_t seq = 0;      // submit のたびに進む
        std::uint64_t usedSeq = 0;  // ミキサーへ渡した最後（音声のスレッド）
        std::chrono::steady_clock::time_point lastSubmit{};
        std::atomic<double> latency{0.05};
        bool primed = false;  // 遅れの最初の値を受けた（なめらかにし始める）
    };
    void loop();

    AudioClipCache& clips_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace audio
}  // namespace wizengine
