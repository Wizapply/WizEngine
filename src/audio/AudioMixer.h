#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "audio/AudioClip.h"
#include "audio/AudioFrame.h"

// 1 つの耳（= 1 つのカメラのページ）のミキサー。音声のスレッド（audio/AudioOutput）
// だけが触る。Chrono も Filament も GStreamer も知らない。
//
//   update(frame) … 描画の 1 フレームぶんの状態を受ける（耳と音源の位置・設計値・
//                   Play / Stop の回数）。Play / Stop をここで声に直す。
//   render(out, n) … 48 kHz ステレオの n フレームを足し合わせて int16 で書く。
//
// 1 つの音源の音（Unity と同じ考え方）:
//   2D の分 = (1 − spatialBlend) × volume、panStereo で左右のバランス
//             （中央で左右とも 1 = クリップのステレオをそのまま）
//   3D の分 = spatialBlend × volume × 距離の減衰、左右はリスナーの右向きへの
//             成分で決める等パワーのパン（クリップは左右を足して 1 本にする）
//   距離の減衰 = logarithmic: d <= min で 1、その先 min / d（max より先は max の値）
//               linear:      d <= min で 1、max で 0 へ直線
//   ドップラー = (c + v_耳·n) / (c + v_音源·n)（n = 耳 → 音源の向き、c = 音速）を
//               doppler × 耳の doppler 倍して、3D の割合だけ効かせる
// パラメータ（左右の音量・再生速度）はブロックの中で前の値から直線で動かす
// （フレームごとの段差でプチプチ鳴らさない）。
//
// Play / Stop は世代（開始・停止・Reset で進む）ごとの回数の差で拾う:
//   世代が変わった → 全部止め、回数を 0 から数え直し、playOnAwake を鳴らす
//   設計の版が変わった → 今の回数から数え直す（番号がずれても前の回数で鳴らさない）
//   loop の音源は鳴っていれば Play しても鳴らし直さない / loop でない音源は重ねる
//   （1 音源 8 声まで、1 回の update で増やすのは 3 声まで）
// クリップがまだ読めていなければ、loop は読めた時点で鳴らし、一発の音は 0.5 秒
// 以内に読めたときだけ鳴らす。
//
// **音の頭の割り付け**（時刻付きの Play。制御器の audio:<名前>.trigger / .play と
// イベントの playAudio）: Play の回数は描画のフレームごとにしか届かないので、そのまま
// 鳴らすと音の頭がフレーム（約 16.7 ms）に丸まる。6800 rpm の V6 の点火（340 回 / 秒）
// では 1 フレームに 5〜6 回ぶんが同じ瞬間に重なってリズムが消える。そこで Play ごとの
// シミュレーション時刻（AudioSourceState::playTimes）を、出力のサンプル位置へ写して
// 鳴らす: 写像「シミュレーション時刻 → 出力のサンプル位置」は、フレームごとの
// （そのフレームの simTime, そのとき出し始めるサンプル位置）の組から α-β フィルタで
// 推定する（位置と傾き = 1 シミュレーション秒あたりのサンプル数。物理が実時間より
// 遅い・スローモーションでも傾きが合う。フレームが届く時刻の揺れ（ブロック 10 ms の
// 境目まで待つ）はフィルタがならす）。音の頭は「写した位置 + 余裕」から鳴り始め、声は
// そのサンプルまで黙って待つ。余裕はジッタバッファと同じく、遅れて届いた Play を見て
// 広げ（10〜100 ms）、ゆっくり狭める = 60 fps の描画でも並びが崩れない（音は余裕の
// ぶん映像より遅れる。数十 ms）。
// 時間の速さが変わったら傾きをその比で直す（スローモーションの切り替え）。一時停止
// の間は出力の位置も止める（待っている声がまとめて鳴らない）。
namespace wizengine {
namespace audio {

class AudioMixer {
public:
    // クリップの取り出し口（AudioClipCache::get）。まだ読めていなければ null。
    using ClipSource = std::function<std::shared_ptr<const AudioClip>(const std::string&)>;

    explicit AudioMixer(ClipSource clips);

    void update(const AudioFrame& frame);
    // out: 48 kHz ステレオの交互配置 n フレーム。
    void render(std::int16_t* out, std::size_t frames);

    // 鳴っている声の数（確認用）。
    std::size_t voiceCount() const;
    // 直前の render の最大振幅（0..1、確認用）。
    float lastPeak() const { return lastPeak_; }

    static constexpr int kMaxVoices = 8;
    static constexpr int kMaxNewPerUpdate = 3;  // 時刻の無い Play（時刻付きは kMaxVoices まで）

    // 出した音の長さ（シミュレート中で一時停止していない間のサンプル数。確認用）。
    std::uint64_t renderedFrames() const { return rendered_; }
    // シミュレーション時刻 t の音の頭を置くサンプル位置（renderedFrames と同じ数え方）。
    // 写像がまだ無ければ -1（確認用）。
    double samplePositionOf(double simTime) const;

private:
    struct Voice {
        std::shared_ptr<const AudioClip> clip;
        double pos = 0.0;  // クリップの中の位置（フレーム、端数あり）
        bool loop = false;
        std::uint64_t startAt = 0;  // 鳴り始めるサンプル位置（rendered_ の数え方）。それまで黙る
    };
    struct Source {
        std::string clip;
        bool loop = false;
        std::vector<Voice> voices;
        std::uint64_t play = 0, stop = 0;
        bool awakePending = false;
        int pendingOneShots = 0;
        double pendingSince = 0.0;  // 一発の音を頼んだ時刻（mixer の時計 s）
        bool pendingLoop = false;
        // 位置と速度（前のフレームとの差）。
        AudioVec3 pos;
        AudioVec3 vel;
        bool hasPos = false;
        // 目標値（update が決める）と、いま出している値（render がなめらかに寄せる）。
        float target2L = 0.0f, target2R = 0.0f, target3L = 0.0f, target3R = 0.0f;
        double targetRate = 1.0;
        float cur2L = 0.0f, cur2R = 0.0f, cur3L = 0.0f, cur3R = 0.0f;
        double curRate = 1.0;
        bool fresh = true;  // 最初の render は目標値から始める（0 からの立ち上がりで鳴らない）
    };

    void startVoice(Source& s, const std::shared_ptr<const AudioClip>& clip, bool loop,
                    std::uint64_t startAt = 0);
    // simTime: Play の時刻（NaN = 今すぐ）。
    void requestPlay(Source& s, const AudioSourceState& st, double simTime);
    // 写像（シミュレーション時刻 → サンプル位置）をこのフレームで直す。
    void updateClock(const AudioFrame& f);
    // Play の時刻 → 鳴り始めるサンプル位置（過ぎていれば今 = rendered_）。遅れて届いた
    // ぶんを見て余裕（lead_）を広げる。
    std::uint64_t scheduleAt(double simTime);

    ClipSource clips_;
    std::vector<Source> sources_;
    std::vector<AudioSourceState> states_;  // 直前の update の設計値
    bool haveFrame_ = false;
    bool simulating_ = false;
    bool paused_ = false;
    std::uint64_t generation_ = ~std::uint64_t(0);
    std::uint64_t version_ = ~std::uint64_t(0);
    double prevSimTime_ = 0.0;
    AudioVec3 listenerPos_, listenerVel_;
    bool hasListener_ = false;
    float masterTarget_ = 1.0f, masterCur_ = 1.0f;
    double clock_ = 0.0;  // 出した音の長さ（s）。一発の音の待ち時間に使う
    std::uint64_t rendered_ = 0;  // 出したサンプル数（シミュレート中・一時停止でない間だけ進む）
    // 写像（α-β フィルタ）: simTime = mapSim_ のときサンプル位置 mapPos_、傾き mapRate_
    // （サンプル / シミュレーション秒）。mapCount_ = 直してきた回数（初めは大きな利得）。
    bool mapValid_ = false;
    double mapSim_ = 0.0, mapPos_ = 0.0, mapRate_ = 48000.0, mapScale_ = 1.0;
    int mapCount_ = 0;
    double lead_ = 0.030 * 48000.0;  // 音の頭の余裕（サンプル。AudioMixer.cpp の kLead*）
    float lastPeak_ = 0.0f;
    std::vector<float> mix_;  // 作業領域（ステレオ float）
};

}  // namespace audio
}  // namespace wizengine
