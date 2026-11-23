#include "audio/AudioOutput.h"

#include <algorithm>
#include <cmath>

#include "core/Log.h"

namespace wizengine {
namespace audio {

AudioOutput::AudioOutput(std::size_t cameras, AudioClipCache& clips) : clips_(clips) {
    for (std::size_t i = 0; i < cameras; ++i) {
        auto s = std::make_unique<Slot>();
        s->mixer = std::make_unique<AudioMixer>(
            [this](const std::string& clip) { return clips_.get(clip); });
        slots_.push_back(std::move(s));
    }
}

AudioOutput::~AudioOutput() { stop(); }

void AudioOutput::setSink(std::size_t cam, Sink sink) {
    if (cam < slots_.size()) slots_[cam]->sink = std::move(sink);
}

void AudioOutput::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { loop(); });
}

void AudioOutput::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void AudioOutput::submit(std::size_t cam, AudioFrame frame) {
    if (cam >= slots_.size()) return;
    Slot& s = *slots_[cam];
    std::lock_guard<std::mutex> lk(s.mutex);
    s.frame = std::move(frame);
    s.hasFrame = true;
    ++s.seq;
    s.lastSubmit = std::chrono::steady_clock::now();
}

void AudioOutput::setVideoLatency(std::size_t cam, double seconds) {
    if (cam >= slots_.size() || !std::isfinite(seconds) || seconds < 0.0 || seconds > 1.0) return;
    Slot& s = *slots_[cam];
    // 1 フレームごとの揺れ（描画の間隔・読み戻しの完了の待ち）をならす。sink は PTS を
    // 大きくずれたときだけ付け直すので、ここでの小さな揺れは音に出ない。
    if (!s.primed) {
        s.latency.store(seconds);
        s.primed = true;
    } else {
        const double old = s.latency.load();
        s.latency.store(old + (seconds - old) * 0.05);
    }
}

void AudioOutput::loop() {
    logging::setThreadName("audio");
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    std::uint64_t produced = 0;  // 作ったフレーム数（全カメラ共通の時計）
    std::vector<std::int16_t> pcm(kBlockFrames * kChannels);
    AudioFrame frame;
    while (running_.load()) {
        const auto now = clock::now();
        const double elapsed = std::chrono::duration<double>(now - start).count();
        const auto due = std::uint64_t(elapsed * kSampleRate);
        if (due > produced + kSampleRate / 5) {
            // 0.2 秒より遅れた（重い読み込み・スリープからの復帰）: 追いつかずに飛ばす。
            produced = due - kBlockFrames;
        }
        while (produced + kBlockFrames <= due) {
            for (auto& sp : slots_) {
                Slot& s = *sp;
                if (!s.sink) continue;
                bool live = false, fresh = false;
                {
                    std::lock_guard<std::mutex> lk(s.mutex);
                    live = s.hasFrame && (now - s.lastSubmit) < std::chrono::seconds(1);
                    if (live && s.seq != s.usedSeq) {
                        frame = s.frame;  // 新しく描いたフレームの状態
                        s.usedSeq = s.seq;
                        fresh = true;
                    }
                }
                if (!live) continue;  // 視聴者のいないカメラは回さない
                if (fresh) s.mixer->update(frame);
                s.mixer->render(pcm.data(), kBlockFrames);
                s.sink(pcm.data(), kBlockFrames, s.latency.load());
            }
            produced += kBlockFrames;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
}

}  // namespace audio
}  // namespace wizengine
