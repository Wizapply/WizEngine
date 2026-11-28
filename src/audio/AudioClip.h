#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 音のクリップ（復号済みの PCM）と、その読み込み。
//   ・中身は常に 48 kHz・ステレオ・int16 の交互配置（ミキサーの出力と同じレート =
//     再生中に標本化周波数を変換しない。ステレオなのは 2D の音（BGM）の左右を保つため。
//     3D の音は再生時に左右を足して 1 本にする）。float でなく int16 なのは 5 分の
//     BGM でも 57 MB に収めるため。
//   ・WAV（PCM 8 / 16 / 24 / 32 bit・float 32 bit、任意のチャンネル数とレート）は
//     ここで読む（依存なし）。それ以外（.ogg .mp3 .flac .m4a …）は setFallbackDecoder で
//     渡された関数（main が GStreamer の decodebin を渡す = streaming/AudioDecode）。
//   ・AudioClipCache は**別スレッドで**読む（ミキサーは音声のスレッドで回るので、
//     ファイルを読んで止まってはいけない）。get はまだ読めていなければ null を返して
//     読み込みを頼むだけ。
namespace wizengine {
namespace audio {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
// 1 クリップの長さの上限（10 分。それより長いぶんは切る = 警告）。
constexpr std::size_t kMaxClipFrames = std::size_t(kSampleRate) * 600;

struct AudioClip {
    std::vector<std::int16_t> pcm;  // 48 kHz ステレオの交互配置
    std::size_t frames() const { return pcm.size() / kChannels; }
};

// 復号した float の交互配置（任意のチャンネル数・レート）→ 48 kHz ステレオ int16。
// 1 チャンネルは左右へ同じ値、3 チャンネル以上は先頭の 2 つ。レート変換は線形補間。
AudioClip makeClip(const std::vector<float>& interleaved, int channels, int sampleRate,
                   bool* truncated = nullptr);

// WAV のバイト列を読む（RIFF / WAVE、fmt と data のチャンク。WAVE_FORMAT_EXTENSIBLE も）。
// 読めなければ false と理由（英語）。
bool decodeWav(const std::vector<std::uint8_t>& bytes, AudioClip& out, std::string& error);

// WAV 以外のファイルを読む関数（パスは解決済み = assets/ を前に付けた物）。
using ClipDecoder = std::function<bool(const std::string& path, AudioClip& out,
                                       std::string& error)>;

class AudioClipCache {
public:
    // root: クリップのパスの前に付けるフォルダ（"assets"）。
    explicit AudioClipCache(std::string root);
    ~AudioClipCache();

    AudioClipCache(const AudioClipCache&) = delete;
    AudioClipCache& operator=(const AudioClipCache&) = delete;

    void setFallbackDecoder(ClipDecoder decoder);

    // 読めていればそのクリップ、まだなら null（読み込みを頼む）。読めなかった
    // クリップも null（1 回だけ LOGW。setStale で読み直す）。どのスレッドからでも。
    std::shared_ptr<const AudioClip> get(const std::string& clip);
    // 読めなかった・古くなったクリップを捨てる（次の get で読み直す）。全部 = 空文字列。
    void forget(const std::string& clip = std::string());

private:
    struct Entry {
        std::shared_ptr<const AudioClip> clip;
        bool failed = false;
        bool queued = false;
    };
    void workerLoop();
    bool load(const std::string& clip, AudioClip& out, std::string& error);

    std::string root_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, Entry> entries_;
    std::deque<std::string> queue_;
    ClipDecoder fallback_;
    bool quit_ = false;
    std::thread worker_;
};

}  // namespace audio
}  // namespace wizengine
