#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/AssetFileRule.h"
#include "core/Clamp.h"
#include "core/SafeName.h"
#include "document/EditorTypes.h"

// オーディオ（Unity の AudioSource / AudioListener に相当）。どちらも物理にも描画にも
// 関わらない「音を鳴らす口」。音はサーバーで作る: 描画のスレッドがフレームごとに
// 耳と音源の状態を作り（scene/SceneAudio.cpp）、audio/AudioMixer がカメラ（ページ）ごとに
// 混ぜて、映像と同じ WebRTC の回線で Opus の音声トラックとして送る（映像と同じ時計 =
// 絵と音が揃う）。
//
//   AudioSource   … 付け先（body、-1 = ワールド）のローカルの点にある音源。clip は
//                   assets/ からの相対パス（.wav はエンジンが、.ogg .mp3 などは GStreamer が復号する）。
//                   spatialBlend = 0 が 2D（距離・向きに関係なく鳴る = BGM・UI 音。
//                   panStereo で左右）、1 が 3D（リスナーからの距離で減衰し、向きで
//                   左右に振れる。ドップラー効果）。間の値は Unity と同じく 2 つを混ぜる。
//                   距離の減衰（rolloff）は Unity と同じ 2 通り:
//                     logarithmic … d <= min で 1、その先は min / d（max より先は max の値のまま）
//                     linear      … d <= min で 1、max で 0 へ直線
//   AudioListener … 耳。既定（body = -1）は**そのページのカメラ**（Unity のカメラに付いた
//                   AudioListener。カメラごとにブラウザが 1 つなので、聞こえ方もページごと）。
//                   body を書けばその物に付く（車の運転席で聞く、など）。volume が全体の
//                   音量（AudioListener.volume）、doppler / soundSpeed はドップラー効果の
//                   強さと音速（Unity の Project Settings > Audio の Doppler Factor 相当）。
//
// 鳴らし方は Unity の Play / PlayOneShot / Stop と同じ 3 通りの口がある:
//   ・playOnAwake  … シミュレート開始（と Reset）で鳴り始める
//   ・イベントのノード playAudio / stopAudio（衝突したら鳴らす、など）
//   ・制御器のアクチュエータ audio:<名前>.play（0 → 1 で鳴らす、1 → 0 で止める）/
//     .volume / .pitch（書いている間はその値が勝つ = エンジン音の高さを回転数で、など）
// loop の音源は鳴っている間に Play しても頭から鳴らし直さない（毎フレームのノードから
// 繋いでも途切れない）。loop でない音源の Play は重ねて鳴らす（PlayOneShot = 衝突音が
// 続けて鳴る）。エディタモードでは鳴らない（Unity の Play モードと同じ）。
//
// 文書では <worldbody> の <audiosource> と <audiolistener>（SceneDocument.cpp）。

namespace wizengine {
namespace editor {

enum class AudioRolloff { Logarithmic, Linear };

inline const char* audioRolloffName(AudioRolloff r) {
    return r == AudioRolloff::Linear ? "linear" : "logarithmic";
}
inline AudioRolloff audioRolloffFromName(const std::string& s, AudioRolloff fallback) {
    if (s == "logarithmic" || s == "log") return AudioRolloff::Logarithmic;
    if (s == "linear") return AudioRolloff::Linear;
    return fallback;
}

// 音のファイルか（拡張子）。assets/ の一覧とクリップの配信が使う。
inline bool audioClipExtension(const std::string& file) {
    const auto dot = file.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = file.substr(dot + 1);
    for (auto& c : ext) c = char((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    return ext == "wav" || ext == "ogg" || ext == "mp3" || ext == "flac" || ext == "m4a" ||
           ext == "aac" || ext == "opus" || ext == "webm";
}
// 文書・ブラウザから受けてよいクリップのパス（assets/ の下 + 音の拡張子）。
inline bool audioClipAllowed(const std::string& file) {
    return assetFileAllowed(file) && audioClipExtension(file);
}

struct AudioSourceDesc {
    std::string name;               // 制御器の audio:<名前> とノードの element が引く
    int body = -1;                  // 付け先のオブジェクト番号（-1 = ワールド）
    Vec3d position{0.0, 0.0, 0.0};  // 付け先のローカル (m)
    std::string clip;               // assets/ からの相対パス（空 = 鳴らない）
    double volume = 1.0;            // 0..1（Unity と同じ。少しだけ持ち上げられるよう 4 まで）
    double pitch = 1.0;             // 再生速度の倍率（音の高さ）
    bool loop = false;
    bool playOnAwake = true;        // シミュレート開始で鳴らす
    bool mute = false;
    double spatialBlend = 0.0;      // 0 = 2D、1 = 3D（Unity の既定は 0）
    double panStereo = 0.0;         // -1 = 左 .. 1 = 右（2D の分にだけ効く）
    double minDistance = 1.0;       // m（ここまでは減衰しない）
    double maxDistance = 500.0;     // m
    AudioRolloff rolloff = AudioRolloff::Logarithmic;
    double dopplerLevel = 1.0;      // 0 = ドップラー無し
};

struct AudioListenerDesc {
    int body = -1;                  // -1 = そのページのカメラ
    Vec3d position{0.0, 0.0, 0.0};  // 付け先のローカル (m)
    Vec3d rotation{0.0, 0.0, 0.0};  // オイラー角（度）。前 = ローカル -Z、上 = +Y
    double volume = 1.0;            // 全体の音量
    double doppler = 1.0;           // ドップラー効果の強さ（全音源に掛かる）
    double soundSpeed = 343.0;      // 音速 (m/s)
};

constexpr std::size_t kMaxAudioSources = 256;

// 名前の決まり（英数字と _ -）。ノードの element（sanitizeSafeName）と同じ規則にして
// おかないと、ノードからの名指しが届かない。
inline std::string sanitizeAudioName(const std::string& s) { return sanitizeSafeName(s); }

// 有限でない値（NaN と ±∞）は既定値。パスの決まりに外れたクリップは空（鳴らない）。
inline AudioSourceDesc clampAudioSource(AudioSourceDesc a) {
    const AudioSourceDesc d;
    const auto cl = clampOrFallback;
    a.name = sanitizeAudioName(a.name);
    if (!a.clip.empty() && !audioClipAllowed(a.clip)) a.clip.clear();
    a.volume = cl(a.volume, 0.0, 4.0, d.volume);
    a.pitch = cl(a.pitch, 0.05, 4.0, d.pitch);
    a.spatialBlend = cl(a.spatialBlend, 0.0, 1.0, d.spatialBlend);
    a.panStereo = cl(a.panStereo, -1.0, 1.0, d.panStereo);
    a.minDistance = cl(a.minDistance, 0.01, 100000.0, d.minDistance);
    a.maxDistance = cl(a.maxDistance, 0.02, 100000.0, d.maxDistance);
    if (a.maxDistance <= a.minDistance) a.maxDistance = a.minDistance * 2.0;
    a.dopplerLevel = cl(a.dopplerLevel, 0.0, 5.0, d.dopplerLevel);
    if (a.body < -1) a.body = -1;
    return a;
}
inline AudioListenerDesc clampAudioListener(AudioListenerDesc l) {
    const AudioListenerDesc d;
    const auto cl = clampOrFallback;
    l.volume = cl(l.volume, 0.0, 4.0, d.volume);
    l.doppler = cl(l.doppler, 0.0, 5.0, d.doppler);
    l.soundSpeed = cl(l.soundSpeed, 1.0, 100000.0, d.soundSpeed);
    if (l.body < -1) l.body = -1;
    return l;
}

// ---- JSON（/scene の audio とブラウザの edit.audio.* のキー）------------------------

inline nlohmann::json toJson(const AudioSourceDesc& a) {
    nlohmann::json j;
    j["name"] = a.name;
    j["body"] = a.body;
    j["pos"] = toJson(a.position);
    j["clip"] = a.clip;
    j["volume"] = a.volume;
    j["pitch"] = a.pitch;
    j["loop"] = a.loop;
    j["playOnAwake"] = a.playOnAwake;
    j["mute"] = a.mute;
    j["spatialBlend"] = a.spatialBlend;
    j["panStereo"] = a.panStereo;
    j["minDistance"] = a.minDistance;
    j["maxDistance"] = a.maxDistance;
    j["rolloff"] = audioRolloffName(a.rolloff);
    j["dopplerLevel"] = a.dopplerLevel;
    return j;
}
// 送られてきたキーだけ base を上書きする（部分更新）。型の違う値は無視する。
inline AudioSourceDesc audioSourceFromJson(const nlohmann::json& j, const AudioSourceDesc& base) {
    AudioSourceDesc a = base;
    if (!j.is_object()) return a;
    a.name = jsonString(j, "name", a.name);
    a.body = jsonInt(j, "body", a.body);
    if (j.contains("pos")) a.position = vec3FromJson(j["pos"], a.position);
    a.clip = jsonString(j, "clip", a.clip);
    a.volume = jsonNumber(j, "volume", a.volume);
    a.pitch = jsonNumber(j, "pitch", a.pitch);
    if (j.contains("loop") && j["loop"].is_boolean()) a.loop = j["loop"];
    if (j.contains("playOnAwake") && j["playOnAwake"].is_boolean()) a.playOnAwake = j["playOnAwake"];
    if (j.contains("mute") && j["mute"].is_boolean()) a.mute = j["mute"];
    a.spatialBlend = jsonNumber(j, "spatialBlend", a.spatialBlend);
    a.panStereo = jsonNumber(j, "panStereo", a.panStereo);
    a.minDistance = jsonNumber(j, "minDistance", a.minDistance);
    a.maxDistance = jsonNumber(j, "maxDistance", a.maxDistance);
    a.rolloff = audioRolloffFromName(jsonString(j, "rolloff", ""), a.rolloff);
    a.dopplerLevel = jsonNumber(j, "dopplerLevel", a.dopplerLevel);
    return a;
}

inline nlohmann::json toJson(const AudioListenerDesc& l) {
    nlohmann::json j;
    j["body"] = l.body;
    j["pos"] = toJson(l.position);
    j["rot"] = toJson(l.rotation);
    j["volume"] = l.volume;
    j["doppler"] = l.doppler;
    j["soundSpeed"] = l.soundSpeed;
    return j;
}
inline AudioListenerDesc audioListenerFromJson(const nlohmann::json& j,
                                               const AudioListenerDesc& base) {
    AudioListenerDesc l = base;
    if (!j.is_object()) return l;
    l.body = jsonInt(j, "body", l.body);
    if (j.contains("pos")) l.position = vec3FromJson(j["pos"], l.position);
    if (j.contains("rot")) l.rotation = vec3FromJson(j["rot"], l.rotation);
    l.volume = jsonNumber(j, "volume", l.volume);
    l.doppler = jsonNumber(j, "doppler", l.doppler);
    l.soundSpeed = jsonNumber(j, "soundSpeed", l.soundSpeed);
    return l;
}

}  // namespace editor
}  // namespace wizengine
