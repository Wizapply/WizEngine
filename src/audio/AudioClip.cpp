#include "audio/AudioClip.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

#include "core/Log.h"

namespace wizengine {
namespace audio {

namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}
std::uint16_t le16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }

std::int16_t toS16(float v) {
    if (!(v == v)) v = 0.0f;  // NaN
    v = std::max(-1.0f, std::min(1.0f, v));
    return std::int16_t(std::lround(v * 32767.0f));
}

}  // namespace

AudioClip makeClip(const std::vector<float>& in, int channels, int sampleRate, bool* truncated) {
    AudioClip clip;
    if (truncated) *truncated = false;
    if (channels <= 0 || sampleRate <= 0) return clip;
    const std::size_t srcFrames = in.size() / std::size_t(channels);
    if (srcFrames == 0) return clip;
    // 元の 1 フレーム → 左右の 2 値。
    auto frameLR = [&](std::size_t i, float& l, float& r) {
        const float* f = &in[i * std::size_t(channels)];
        l = f[0];
        r = channels >= 2 ? f[1] : f[0];
    };
    const double ratio = double(sampleRate) / double(kSampleRate);  // 出力 1 標本あたりの元の標本
    std::size_t outFrames =
        sampleRate == kSampleRate ? srcFrames
                                  : std::size_t(std::floor(double(srcFrames - 1) / ratio)) + 1;
    if (outFrames > kMaxClipFrames) {
        outFrames = kMaxClipFrames;
        if (truncated) *truncated = true;
    }
    clip.pcm.resize(outFrames * kChannels);
    for (std::size_t o = 0; o < outFrames; ++o) {
        float l = 0.0f, r = 0.0f;
        if (sampleRate == kSampleRate) {
            frameLR(o, l, r);
        } else {
            const double s = double(o) * ratio;
            const std::size_t i0 = std::size_t(s);
            const std::size_t i1 = std::min(i0 + 1, srcFrames - 1);
            const float t = float(s - double(i0));
            float l0, r0, l1, r1;
            frameLR(i0, l0, r0);
            frameLR(i1, l1, r1);
            l = l0 + (l1 - l0) * t;
            r = r0 + (r1 - r0) * t;
        }
        clip.pcm[o * 2] = toS16(l);
        clip.pcm[o * 2 + 1] = toS16(r);
    }
    return clip;
}

bool decodeWav(const std::vector<std::uint8_t>& b, AudioClip& out, std::string& error) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 ||
        std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }
    int format = 0, channels = 0, rate = 0, bits = 0;
    const std::uint8_t* data = nullptr;
    std::size_t dataSize = 0;
    std::size_t pos = 12;
    while (pos + 8 <= b.size()) {
        const std::uint8_t* h = b.data() + pos;
        const std::size_t size = le32(h + 4);
        const std::size_t body = pos + 8;
        const std::size_t avail = b.size() - body;
        if (std::memcmp(h, "fmt ", 4) == 0 && size >= 16 && avail >= 16) {
            format = le16(b.data() + body);
            channels = le16(b.data() + body + 2);
            rate = int(le32(b.data() + body + 4));
            bits = le16(b.data() + body + 14);
            // WAVE_FORMAT_EXTENSIBLE: 本当の形式は SubFormat の先頭 2 バイト。
            if (format == 0xFFFE && size >= 26 && avail >= 26) format = le16(b.data() + body + 24);
        } else if (std::memcmp(h, "data", 4) == 0) {
            data = b.data() + body;
            dataSize = std::min(size, avail);  // 長さが壊れている（録音中断）ものも読めるだけ
            break;
        }
        pos = body + size + (size & 1);  // チャンクは偶数境界
    }
    if (!data || channels <= 0 || rate <= 0) {
        error = "no fmt/data chunk";
        return false;
    }
    const bool isFloat = format == 3;
    if (!(format == 1 || isFloat) || (isFloat && bits != 32) ||
        (!isFloat && bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
        error = "unsupported WAV encoding (format " + std::to_string(format) + ", " +
                std::to_string(bits) + " bit) - use PCM 8/16/24/32 bit or float 32 bit";
        return false;
    }
    const std::size_t bytesPer = std::size_t(bits / 8);
    const std::size_t count = dataSize / bytesPer;
    std::vector<float> f(count - count % std::size_t(channels));
    for (std::size_t i = 0; i < f.size(); ++i) {
        const std::uint8_t* p = data + i * bytesPer;
        float v = 0.0f;
        if (isFloat) {
            std::memcpy(&v, p, 4);
        } else if (bits == 8) {
            v = (float(p[0]) - 128.0f) / 128.0f;
        } else if (bits == 16) {
            v = float(std::int16_t(le16(p))) / 32768.0f;
        } else if (bits == 24) {
            std::int32_t s = std::int32_t(p[0] | (p[1] << 8) | (p[2] << 16));
            if (s & 0x800000) s |= ~0xFFFFFF;
            v = float(s) / 8388608.0f;
        } else {
            v = float(std::int32_t(le32(p))) / 2147483648.0f;
        }
        f[i] = v;
    }
    bool truncated = false;
    out = makeClip(f, channels, rate, &truncated);
    if (truncated) error = "longer than 10 minutes - truncated";
    return !out.pcm.empty() || f.empty();
}

// ---- AudioClipCache -------------------------------------------------------------------

AudioClipCache::AudioClipCache(std::string root) : root_(std::move(root)) {
    worker_ = std::thread([this] { workerLoop(); });
}

AudioClipCache::~AudioClipCache() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void AudioClipCache::setFallbackDecoder(ClipDecoder decoder) {
    std::lock_guard<std::mutex> lk(mutex_);
    fallback_ = std::move(decoder);
}

std::shared_ptr<const AudioClip> AudioClipCache::get(const std::string& clip) {
    if (clip.empty()) return nullptr;
    std::lock_guard<std::mutex> lk(mutex_);
    Entry& e = entries_[clip];
    if (e.clip) return e.clip;
    if (!e.failed && !e.queued) {
        e.queued = true;
        queue_.push_back(clip);
        cv_.notify_one();
    }
    return nullptr;
}

void AudioClipCache::forget(const std::string& clip) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (clip.empty()) {
        // 読み込み中のものは残す（終わったら入る）。
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.queued) ++it;
            else it = entries_.erase(it);
        }
    } else {
        auto it = entries_.find(clip);
        if (it != entries_.end() && !it->second.queued) entries_.erase(it);
    }
}

bool AudioClipCache::load(const std::string& clip, AudioClip& out, std::string& error) {
    const std::string path = root_.empty() ? clip : root_ + "/" + clip;
    std::string ext;
    if (const auto dot = clip.find_last_of('.'); dot != std::string::npos) {
        ext = clip.substr(dot + 1);
        for (auto& c : ext) c = char((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    if (ext == "wav") {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "cannot open " + path;
            return false;
        }
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        return decodeWav(bytes, out, error);
    }
    ClipDecoder dec;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        dec = fallback_;
    }
    if (!dec) {
        error = "only .wav can be decoded in this build (no decoder for ." + ext + ")";
        return false;
    }
    return dec(path, out, error);
}

void AudioClipCache::workerLoop() {
    for (;;) {
        std::string clip;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            clip = queue_.front();
            queue_.pop_front();
        }
        auto loaded = std::make_shared<AudioClip>();
        std::string error;
        const bool ok = load(clip, *loaded, error);
        if (ok) {
            LOGI("audio", "loaded %s (%.2f s)%s%s", clip.c_str(),
                 double(loaded->frames()) / kSampleRate, error.empty() ? "" : " - ",
                 error.c_str());
        } else {
            LOGW("audio", "could not load %s: %s", clip.c_str(), error.c_str());
        }
        std::lock_guard<std::mutex> lk(mutex_);
        Entry& e = entries_[clip];
        e.queued = false;
        if (ok) e.clip = std::move(loaded);
        else e.failed = true;
    }
}

}  // namespace audio
}  // namespace wizengine
