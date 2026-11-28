#include "audio/AudioMixer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wizengine {
namespace audio {

namespace {

constexpr double kPi = 3.14159265358979323846;

AudioVec3 sub(const AudioVec3& a, const AudioVec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot(const AudioVec3& a, const AudioVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
AudioVec3 cross(const AudioVec3& a, const AudioVec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double length(const AudioVec3& a) { return std::sqrt(dot(a, a)); }
AudioVec3 scale(const AudioVec3& a, double k) { return {a.x * k, a.y * k, a.z * k}; }
bool finite(const AudioVec3& a) {
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}

// 前のフレームとの差からの速度。置き直しの跳びをドップラーにしない（音速の半分で
// 頭打ち）、時間が進んでいなければ前の値のまま。
AudioVec3 velocityFrom(const AudioVec3& prev, const AudioVec3& cur, double dt,
                       const AudioVec3& old) {
    if (!(dt > 1e-6)) return old;
    AudioVec3 v = scale(sub(cur, prev), 1.0 / dt);
    const double n = length(v);
    if (!std::isfinite(n)) return AudioVec3{};
    if (n > 170.0) v = scale(v, 170.0 / n);
    // 少しならす（描画の補間の段差で速度が跳ねない）。
    return {old.x + (v.x - old.x) * 0.5, old.y + (v.y - old.y) * 0.5, old.z + (v.z - old.z) * 0.5};
}

// 音の頭の割り付け（AudioMixer.h の冒頭）。
// 余裕（写した位置から何秒後ろへずらして鳴らすか）はジッタバッファと同じ考え方で決める:
// Play が届くのは「その時刻の絵を描くより少し前」〜「描いた後のフレーム + ブロック」の間で
// ばらつくので、遅れて届いた（写した位置が過ぎていた）ぶんを見て余裕を広げ、ゆっくり
// 狭める。広げた瞬間だけ間隔が 1 つ伸び、その後は並びが崩れない。音は映像より余裕の
// ぶんだけ遅れる（数十 ms = 人が気づく遅れ（約 125 ms）より十分小さい）。
constexpr double kLeadMin = 0.010;        // 余裕の下限（s）
constexpr double kLeadInit = 0.030;       // 最初の余裕（s）= 60 fps のフレーム + ブロックの目安
constexpr double kLeadMax = 0.100;        // 余裕の上限（s）
constexpr double kLeadMargin = 0.004;     // 遅れて届いたとき、その遅れに足す分（s）
constexpr double kLeadDecay = 0.0008;     // 1 フレームごとに下限へ寄せる割合（約 20 s で 1/e）
constexpr double kScheduleMax = 0.5;      // これより先へは置かない（写像が狂ったときの安全弁、s）
constexpr double kMapAlpha = 0.1;         // α-β フィルタの定常の利得（位置）
constexpr double kMapBeta = kMapAlpha * kMapAlpha / (2.0 - kMapAlpha);  // 同（傾き）= 臨界減衰
constexpr double kMapReset = 0.15;        // 予測とこれ以上ずれたら付け直す（s）

double attenuation(const AudioSourceState& s, double d) {
    const double mn = s.minDistance, mx = s.maxDistance;
    if (d <= mn) return 1.0;
    if (s.linearRolloff) return d >= mx ? 0.0 : 1.0 - (d - mn) / (mx - mn);
    return mn / std::min(d, mx);
}

}  // namespace

AudioMixer::AudioMixer(ClipSource clips) : clips_(std::move(clips)) {}

std::size_t AudioMixer::voiceCount() const {
    std::size_t n = 0;
    for (const auto& s : sources_) n += s.voices.size();
    return n;
}

void AudioMixer::startVoice(Source& s, const std::shared_ptr<const AudioClip>& clip, bool loop,
                            std::uint64_t startAt) {
    if (!clip || clip->frames() == 0) return;
    if (int(s.voices.size()) >= kMaxVoices) s.voices.erase(s.voices.begin());  // 古い声を譲る
    Voice v;
    v.clip = clip;
    v.loop = loop;
    v.startAt = startAt;
    s.voices.push_back(std::move(v));
}

double AudioMixer::samplePositionOf(double simTime) const {
    if (!mapValid_ || !std::isfinite(simTime)) return -1.0;
    return mapPos_ + (simTime - mapSim_) * mapRate_;
}

std::uint64_t AudioMixer::scheduleAt(double simTime) {
    const double p = samplePositionOf(simTime);
    if (!(p >= 0.0)) return rendered_;  // 時刻なし・写像なし: 今すぐ
    const double now = double(rendered_);
    // 遅れて届いた（余裕を足しても過ぎている）: 余裕をその遅れ + 少しへ広げる。上限より
    // 遅れたもの（救えない外れ値 = 描画が長く止まった等）では広げない（その後 20 秒ほど
    // 全部の音が遅れたままになるため）。
    const double late = now - p;
    if (late < kLeadMax * kSampleRate && late + kLeadMargin * kSampleRate > lead_) {
        lead_ = std::min(kLeadMax * kSampleRate, late + kLeadMargin * kSampleRate);
    }
    const double at = p + lead_;
    if (!(at > now)) return rendered_;  // 余裕の上限を超えて過ぎている: 今すぐ
    return std::uint64_t(std::min(at, now + kScheduleMax * kSampleRate));
}

void AudioMixer::updateClock(const AudioFrame& f) {
    const double S = f.simTime;
    const double P = double(rendered_);  // このフレームの状態で鳴らし始める位置
    const double scale = std::max(0.001, std::min(4.0, f.timeScale > 0.0 ? f.timeScale : 1.0));
    if (!std::isfinite(S)) return;
    if (!mapValid_) {
        mapValid_ = true;
        mapSim_ = S;
        mapPos_ = P;
        mapRate_ = kSampleRate / scale;  // 物理が実時間どおりと仮定して始め、フィルタが直す
        mapScale_ = scale;
        mapCount_ = 1;
        return;
    }
    if (std::fabs(scale / mapScale_ - 1.0) > 1e-3) {
        // 時間の速さが変わった: 傾きをその比で直して、ここから付け直す。
        mapRate_ *= mapScale_ / scale;
        mapScale_ = scale;
        mapSim_ = S;
        mapPos_ = P;
        return;
    }
    const double dS = S - mapSim_;
    if (!(dS > 1e-9)) return;  // シミュレーション時刻が進んでいない（物理が遅れている）
    const double pred = mapPos_ + dS * mapRate_;
    const double r = P - pred;
    if (std::fabs(r) > kMapReset * kSampleRate) {
        // 大きく外れた（物理が長く止まった・時計が跳んだ）: 位置を付け直す。
        mapSim_ = S;
        mapPos_ = P;
        mapCount_ = 1;
        return;
    }
    // 初めの数回は「ここまでの点の最小二乗」と同じ利得（大きい）から、定常の利得へ
    // 下げていく = 実時間より遅い物理でも数フレームで傾きが合う。
    ++mapCount_;
    const double n = double(mapCount_);
    const double a = std::max(kMapAlpha, 2.0 * (2.0 * n - 1.0) / (n * (n + 1.0)));
    const double b = std::max(kMapBeta, 6.0 / (n * (n + 1.0)));
    mapPos_ = pred + a * r;
    mapRate_ += b * r / dS;
    mapRate_ = std::max(kSampleRate / 8.0, std::min(kSampleRate * 2000.0, mapRate_));
    mapSim_ = S;
}

void AudioMixer::requestPlay(Source& s, const AudioSourceState& st, double simTime) {
    if (st.clip.empty() || !st.alive) return;
    if (st.loop && (!s.voices.empty() || s.pendingLoop)) return;  // 鳴っていれば鳴らし直さない
    auto clip = clips_ ? clips_(st.clip) : nullptr;
    if (clip) {
        startVoice(s, clip, st.loop, scheduleAt(simTime));
        return;
    }
    // まだ読めていない: loop は読めたら鳴らす、一発の音は少しだけ待つ。
    if (st.loop) {
        s.pendingLoop = true;
    } else if (s.pendingOneShots < kMaxNewPerUpdate) {
        if (s.pendingOneShots == 0) s.pendingSince = clock_;
        ++s.pendingOneShots;
    }
}

void AudioMixer::update(const AudioFrame& f) {
    const bool genChanged = !haveFrame_ || f.generation != generation_ || !f.simulating;
    const bool verChanged = f.version != version_;
    const double dt = f.simTime - prevSimTime_;
    paused_ = f.simulating && f.paused;

    // エディタ / 世代が変わった: 全部止める（Unity の Play モードの外では鳴らない）。
    if (!f.simulating || (haveFrame_ && f.generation != generation_)) {
        for (auto& s : sources_) {
            s.voices.clear();
            s.pendingLoop = false;
            s.pendingOneShots = 0;
        }
    }
    // 音の頭の割り付け（シミュレーション時刻 → サンプル位置）。世代が変わったら作り直す
    // （Reset でシミュレーション時刻は戻らないが、止めていた間は写像が合わない）。
    if (genChanged) mapValid_ = false;
    if (f.simulating && !paused_) updateClock(f);
    lead_ -= (lead_ - kLeadMin * kSampleRate) * kLeadDecay;

    // ---- 耳 ----
    const AudioListenerState& L = f.listener;
    if (hasListener_ && f.simulating && !genChanged) {
        listenerVel_ = velocityFrom(listenerPos_, L.pos, dt, listenerVel_);
    } else {
        listenerVel_ = AudioVec3{};
    }
    listenerPos_ = L.pos;
    hasListener_ = finite(L.pos);
    masterTarget_ = float(std::max(0.0, std::min(4.0, L.volume)));
    AudioVec3 fwd = L.fwd, up = L.up;
    if (length(fwd) < 1e-9) fwd = {0.0, 0.0, -1.0};
    if (length(up) < 1e-9) up = {0.0, 1.0, 0.0};
    AudioVec3 right = cross(fwd, up);
    const double rn = length(right);
    right = rn > 1e-9 ? scale(right, 1.0 / rn) : AudioVec3{1.0, 0.0, 0.0};
    const double c = std::max(1.0, L.soundSpeed);

    // ---- 音源の一覧（数・クリップ・loop が変わった物は止める）----
    if (sources_.size() > f.sources.size()) sources_.resize(f.sources.size());
    while (sources_.size() < f.sources.size()) sources_.emplace_back();
    for (std::size_t i = 0; i < f.sources.size(); ++i) {
        const AudioSourceState& st = f.sources[i];
        Source& s = sources_[i];
        const bool fresh = i >= states_.size();
        if (s.clip != st.clip || s.loop != st.loop) {
            s.voices.clear();
            s.pendingLoop = false;
            s.pendingOneShots = 0;
            s.clip = st.clip;
            s.loop = st.loop;
            if (!st.clip.empty() && clips_) clips_(st.clip);  // 先に読み込みを頼む
            // 走っている最中に足した / クリップを替えた loop の音は、開始時と同じく鳴らす。
            if (!genChanged && verChanged && f.simulating && st.loop && st.playOnAwake) {
                s.awakePending = true;
            }
        }
        if (genChanged) {
            s.play = 0;
            s.stop = 0;
            s.awakePending = f.simulating && st.playOnAwake;
            s.hasPos = false;
            s.vel = AudioVec3{};
        } else if (verChanged || fresh) {
            s.play = st.play;
            s.stop = st.stop;
        }
        // 速度。
        if (s.hasPos && f.simulating) s.vel = velocityFrom(s.pos, st.pos, dt, s.vel);
        s.pos = st.pos;
        s.hasPos = finite(st.pos);
    }
    states_ = f.sources;
    generation_ = f.generation;
    version_ = f.version;
    prevSimTime_ = f.simTime;
    simulating_ = f.simulating;
    haveFrame_ = true;
    if (!f.simulating) return;

    // ---- 再生の指示と、このフレームの目標値 ----
    for (std::size_t i = 0; i < f.sources.size(); ++i) {
        const AudioSourceState& st = f.sources[i];
        Source& s = sources_[i];
        if (st.stop > s.stop) {
            s.voices.clear();
            s.pendingLoop = false;
            s.pendingOneShots = 0;
            s.stop = st.stop;
        }
        if (s.awakePending) {
            s.awakePending = false;
            requestPlay(s, st, std::numeric_limits<double>::quiet_NaN());
        }
        if (st.play > s.play) {
            // 増えたぶんの Play（最後の n 回）。時刻があれば音の頭をその時刻へ割り付ける
            // （古い順に足す = 声の並びが時刻順）。時刻付きは 1 フレームに kMaxVoices まで
            // （それより古いぶんは過ぎた音）、時刻なしは kMaxNewPerUpdate まで。
            const std::uint64_t added = st.play - s.play;
            const std::size_t nt = st.playTimes.size();
            const bool timed = nt > 0 && std::isfinite(st.playTimes.back());
            const std::uint64_t cap = st.loop ? 1u : std::uint64_t(timed ? kMaxVoices : kMaxNewPerUpdate);
            const std::uint64_t n = std::min<std::uint64_t>(cap, added);
            for (std::uint64_t k = 0; k < n; ++k) {
                // 最後の n 回の k 番目 = 時刻の列の後ろから (n - k) 番目。
                const std::uint64_t back = n - k;
                const double t = back <= nt ? st.playTimes[nt - std::size_t(back)]
                                            : std::numeric_limits<double>::quiet_NaN();
                requestPlay(s, st, t);
            }
            s.play = st.play;
        }

        const double blend = std::max(0.0, std::min(1.0, st.spatialBlend));
        const double vol = (st.mute || !st.alive) ? 0.0 : std::max(0.0, st.volume);
        // 2D: 中央で左右とも 1、パンの側へ寄せるともう片方が下がる（バランス）。
        const double pan = std::max(-1.0, std::min(1.0, st.panStereo));
        const double g2 = (1.0 - blend) * vol;
        s.target2L = float(g2 * (pan > 0.0 ? 1.0 - pan : 1.0));
        s.target2R = float(g2 * (pan < 0.0 ? 1.0 + pan : 1.0));
        // 3D: 距離の減衰 × 等パワーのパン（右向きへの成分）。
        double g3 = 0.0, x = 0.0, ratio = 1.0;
        if (blend > 0.0 && hasListener_ && s.hasPos) {
            const AudioVec3 d = sub(st.pos, listenerPos_);
            const double dist = length(d);
            g3 = blend * vol * attenuation(st, dist);
            if (dist > 1e-6) {
                const AudioVec3 n = scale(d, 1.0 / dist);
                x = std::max(-1.0, std::min(1.0, dot(n, right)));
                const double k = std::max(0.0, st.dopplerLevel) * std::max(0.0, L.doppler);
                if (k > 0.0) {
                    const double vl = dot(listenerVel_, n) * k;
                    const double vs = dot(s.vel, n) * k;
                    ratio = (c + std::min(vl, c * 0.9)) / (c + std::max(vs, -c * 0.9));
                    ratio = std::max(0.25, std::min(4.0, ratio));
                    ratio = 1.0 + (ratio - 1.0) * blend;
                }
            }
        }
        const double theta = (x + 1.0) * kPi / 4.0;
        s.target3L = float(g3 * std::cos(theta));
        s.target3R = float(g3 * std::sin(theta));
        s.targetRate = std::max(0.01, std::min(8.0, std::max(0.05, st.pitch) * ratio));
    }
}

void AudioMixer::render(std::int16_t* out, std::size_t frames) {
    lastPeak_ = 0.0f;
    if (frames == 0) return;
    mix_.assign(frames * 2, 0.0f);
    const double blockSec = double(frames) / kSampleRate;
    if (!simulating_ || paused_) {
        // エディタ・一時停止: 黙る（一時停止では声の位置も、待っている声の出番も止める）。
        std::fill(out, out + frames * 2, std::int16_t(0));
        if (!paused_) clock_ += blockSec;
        return;
    }
    const std::uint64_t blockStart = rendered_;
    const float inv = 1.0f / float(frames);
    for (std::size_t i = 0; i < sources_.size() && i < states_.size(); ++i) {
        Source& s = sources_[i];
        const AudioSourceState& st = states_[i];
        // 読み込み待ちの音を、読めたら鳴らす。
        if ((s.pendingLoop || s.pendingOneShots > 0) && clips_) {
            if (auto clip = clips_(st.clip)) {
                if (s.pendingLoop && s.voices.empty()) startVoice(s, clip, true);
                if (s.pendingOneShots > 0 && clock_ - s.pendingSince <= 0.5) {
                    for (int k = 0; k < s.pendingOneShots; ++k) startVoice(s, clip, false);
                }
                s.pendingLoop = false;
                s.pendingOneShots = 0;
            } else if (s.pendingOneShots > 0 && clock_ - s.pendingSince > 0.5) {
                s.pendingOneShots = 0;  // 遅れて鳴っても意味が無い
            }
        }
        if (s.fresh) {
            s.cur2L = s.target2L;
            s.cur2R = s.target2R;
            s.cur3L = s.target3L;
            s.cur3R = s.target3R;
            s.curRate = s.targetRate;
            s.fresh = false;
        }
        if (s.voices.empty()) {
            s.cur2L = s.target2L;
            s.cur2R = s.target2R;
            s.cur3L = s.target3L;
            s.cur3R = s.target3R;
            s.curRate = s.targetRate;
            continue;
        }
        const float d2L = (s.target2L - s.cur2L) * inv, d2R = (s.target2R - s.cur2R) * inv;
        const float d3L = (s.target3L - s.cur3L) * inv, d3R = (s.target3R - s.cur3R) * inv;
        const double dRate = (s.targetRate - s.curRate) / double(frames);
        for (auto vit = s.voices.begin(); vit != s.voices.end();) {
            Voice& v = *vit;
            // 出番（割り付けた音の頭）がこのブロックより後なら、まだ黙って待つ。
            std::size_t k0 = 0;
            if (v.startAt > blockStart) {
                const std::uint64_t wait = v.startAt - blockStart;
                if (wait >= frames) {
                    ++vit;
                    continue;
                }
                k0 = std::size_t(wait);
            }
            const std::int16_t* pcm = v.clip->pcm.data();
            const std::size_t n = v.clip->frames();
            const float fk0 = float(k0);
            float g2L = s.cur2L + d2L * fk0, g2R = s.cur2R + d2R * fk0;
            float g3L = s.cur3L + d3L * fk0, g3R = s.cur3R + d3R * fk0;
            double rate = s.curRate + dRate * double(k0);
            bool ended = false;
            for (std::size_t k = k0; k < frames; ++k) {
                std::size_t i0 = std::size_t(v.pos);
                if (i0 >= n) {
                    if (!v.loop) {
                        ended = true;
                        break;
                    }
                    v.pos = std::fmod(v.pos, double(n));
                    i0 = std::size_t(v.pos);
                }
                const std::size_t i1 = (i0 + 1 < n) ? i0 + 1 : (v.loop ? 0 : i0);
                const float t = float(v.pos - double(i0));
                const float l = (float(pcm[i0 * 2]) + (float(pcm[i1 * 2]) - float(pcm[i0 * 2])) * t) *
                                (1.0f / 32768.0f);
                const float r =
                    (float(pcm[i0 * 2 + 1]) + (float(pcm[i1 * 2 + 1]) - float(pcm[i0 * 2 + 1])) * t) *
                    (1.0f / 32768.0f);
                const float mono = 0.5f * (l + r);
                mix_[k * 2] += l * g2L + mono * g3L;
                mix_[k * 2 + 1] += r * g2R + mono * g3R;
                g2L += d2L;
                g2R += d2R;
                g3L += d3L;
                g3R += d3R;
                v.pos += rate;
                rate += dRate;
            }
            if (ended) vit = s.voices.erase(vit);
            else ++vit;
        }
        s.cur2L = s.target2L;
        s.cur2R = s.target2R;
        s.cur3L = s.target3L;
        s.cur3R = s.target3R;
        s.curRate = s.targetRate;
    }
    // 耳の音量と、割れないための軽い頭打ち（1 を超えた分だけ柔らかく潰す）。
    const float dm = (masterTarget_ - masterCur_) * inv;
    float m = masterCur_;
    float peak = 0.0f;
    for (std::size_t k = 0; k < frames; ++k) {
        for (int ch = 0; ch < 2; ++ch) {
            float x = mix_[k * 2 + ch] * m;
            const float a = std::fabs(x);
            if (a > 0.9f) x = (x > 0 ? 1.0f : -1.0f) * (0.9f + 0.1f * std::tanh((a - 0.9f) / 0.1f));
            peak = std::max(peak, std::fabs(x));
            out[k * 2 + ch] = std::int16_t(std::lround(std::max(-1.0f, std::min(1.0f, x)) * 32767.0f));
        }
        m += dm;
    }
    masterCur_ = masterTarget_;
    lastPeak_ = peak;
    clock_ += blockSec;
    rendered_ += frames;
}

}  // namespace audio
}  // namespace wizengine
