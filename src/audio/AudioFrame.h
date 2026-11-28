#pragma once

#include <cstdint>
#include <string>
#include <vector>

// オーディオの 1 フレームぶんの状態（描画の 1 フレーム = 映像の 1 枚と同じ瞬間）。
// RENDER スレッドが Scene::buildAudioFrame でカメラ（ページ）ごとに作り、
// audio/AudioOutput がミキサーへ渡す。Chrono も Filament も GStreamer も知らない値型。
//
// 位置は**描いた姿勢**（Scene の補間した renderPoses）から出すので、映像に映っている
// 物の位置と同じ。速度はミキサーが前のフレームとの差（シミュレーション時刻で割る）から
// 出す（ドップラー）。
namespace wizengine {
namespace audio {

struct AudioVec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

struct AudioListenerState {
    AudioVec3 pos;
    AudioVec3 fwd{0.0, 0.0, -1.0};
    AudioVec3 up{0.0, 1.0, 0.0};
    double volume = 1.0;
    double doppler = 1.0;       // ドップラー効果の強さ（全音源）
    double soundSpeed = 343.0;  // m/s
    int attached = -1;          // -1 = カメラ、>= 0 = オブジェクトに付いている
};

struct AudioSourceState {
    std::string name;
    std::string clip;      // assets/ 相対（空 = 鳴らない）
    bool alive = true;     // 付け先が消えていれば false（鳴らさない）
    AudioVec3 pos;         // ワールド
    double volume = 1.0;   // 制御器の上書き込み
    double pitch = 1.0;    // 同上
    bool loop = false;
    bool playOnAwake = true;
    bool mute = false;
    double spatialBlend = 0.0;  // 0 = 2D、1 = 3D
    double panStereo = 0.0;     // -1..1（2D の分）
    double minDistance = 1.0;
    double maxDistance = 500.0;
    bool linearRolloff = false;  // false = logarithmic
    double dopplerLevel = 1.0;
    std::uint64_t play = 0;  // この世代の Play の回数
    std::uint64_t stop = 0;  // この世代の Stop の回数
    // 最近の Play のシミュレーション時刻（古い順、最後 = play 回目。最大 16 個、時刻の
    // 無い Play は NaN）。ミキサーが音の頭を出力のサンプル位置へ割り付ける。
    std::vector<double> playTimes;
};

struct AudioFrame {
    bool simulating = false;     // エディタでは鳴らさない（Unity の Play モードの外）
    bool paused = false;         // 一時停止: 音も止める（声の位置もそのまま）
    double simTime = 0.0;        // このフレームのシミュレーション時刻（速度の分母・音の頭の割り付け）
    double timeScale = 1.0;      // 時間の速さ（スローモーション）。変わったら割り付けの傾きを直す
    std::uint64_t generation = 0;  // 開始・停止・Reset で進む（Play / Stop の回数の世代）
    std::uint64_t version = 0;     // 設計（音源の一覧）が変わるたびに進む
    AudioListenerState listener;
    std::vector<AudioSourceState> sources;  // 番号 = 文書の音源の番号
};

}  // namespace audio
}  // namespace wizengine
