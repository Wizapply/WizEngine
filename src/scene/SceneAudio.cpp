// オーディオ（Unity の AudioSource / AudioListener）の Scene 側の受け持ち。
//   ・実行中の指示（Play / Stop の回数・制御器の音量と高さの上書き）
//   ・描いたフレームの音の状態（buildAudioFrame。RENDER スレッドがカメラごとに作り、
//     audio/AudioOutput のミキサーが音にして、映像と同じ WebRTC の回線で送る）
//   ・/scene の audio（Inspector / World タブの欄）
//   ・edit.audio.add / set / remove・edit.audiolistener
// 音を作るのはサーバー（audio/AudioMixer）。ここは「何を・いつ・どこで」まで。

#include "scene/SceneInternal.h"

#include <filesystem>

namespace {

using scenemath::Quat;
using scenemath::Vec3;

Quat quatOf(const BodyTransform& t) { return Quat(t.qw, t.qx, t.qy, t.qz); }
Vec3 posOf(const BodyTransform& t) { return Vec3(t.px, t.py, t.pz); }


// 付け先の姿勢 × ローカルの点。付け先が無い（-1・消えた・姿勢が無い）ならワールド。
Vec3 worldPoint(const std::vector<BodyTransform>& poses, int body, const ed::Vec3d& local,
                bool bodyOk) {
    const Vec3 l(local.x, local.y, local.z);
    if (!bodyOk || body < 0 || std::size_t(body) >= poses.size()) return l;
    const BodyTransform& t = poses[std::size_t(body)];
    return posOf(t) + quatOf(t) * l;
}

// assets/ 以下の音のファイル（assets/ 相対、/ 区切り）。実行時のファイル置き場は見ない。
std::vector<std::string> listAudioFiles() {
    namespace fs = std::filesystem;
    std::vector<std::string> found;
    std::error_code ec;
    const fs::path root(wizengine::kAssetRoot);
    if (!fs::exists(root, ec)) return found;
    static const char* kSkip[] = {"web", "scenes", "materials", "terrains", "events",
                                  "formulas", "prefabs"};
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const auto& entry = *it;
        if (it.depth() == 0 && entry.is_directory(ec)) {
            const std::string top = entry.path().filename().string();
            bool skip = false;
            for (const char* s : kSkip) skip = skip || top == s;
            if (skip) {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (!entry.is_regular_file(ec)) continue;
        const std::string rel = fs::relative(entry.path(), root, ec).generic_string();
        if (ec || rel.empty() || !ed::audioClipAllowed(rel)) continue;
        found.push_back(rel);
        if (found.size() >= 1000) break;
    }
    std::sort(found.begin(), found.end());
    return found;
}

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

// ---- 実行中の指示 -------------------------------------------------------------------

void Scene::audioSyncLocked(std::uint64_t generation, std::size_t count) {
    if (generation != audioRtGeneration_) {
        audioRt_.assign(count, AudioRuntime{});
        audioRtGeneration_ = generation;
    } else if (audioRt_.size() < count) {
        audioRt_.resize(count);
    }
}

void Scene::audioCountPlayLocked(AudioRuntime& r, double simTime) {
    ++r.play;
    // 時刻は最近の kAudioPlayTimes 回ぶん（ミキサーは 1 フレームに増えたぶんの最後から
    // 拾う。1 フレームにこれより多く鳴らしても、古いぶんは過ぎた音なので要らない）。
    if (r.playTimes.size() >= kAudioPlayTimes) r.playTimes.erase(r.playTimes.begin());
    r.playTimes.push_back(std::isfinite(simTime) ? simTime
                                                 : std::numeric_limits<double>::quiet_NaN());
}

void Scene::audioPlay(int source, double simTime) {
    if (source < 0) return;
    const std::uint64_t gen = runGeneration();
    const std::size_t n = editor_.audioSources().size();
    if (std::size_t(source) >= n) return;
    std::lock_guard<std::mutex> lk(audioMutex_);
    audioSyncLocked(gen, n);
    audioCountPlayLocked(audioRt_[std::size_t(source)], simTime);
}

void Scene::audioStop(int source) {
    if (source < 0) return;
    const std::uint64_t gen = runGeneration();
    const std::size_t n = editor_.audioSources().size();
    if (std::size_t(source) >= n) return;
    std::lock_guard<std::mutex> lk(audioMutex_);
    audioSyncLocked(gen, n);
    ++audioRt_[std::size_t(source)].stop;
}

void Scene::audioPlayOn(int object, const std::string& name, bool play, double simTime) {
    const auto sources = editor_.audioSources();
    const std::uint64_t gen = runGeneration();
    std::lock_guard<std::mutex> lk(audioMutex_);
    audioSyncLocked(gen, sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const ed::AudioSourceDesc& a = sources[i];
        if (!name.empty() && a.name != name) continue;
        // 名前で引くときは付け先を問わない（object = -1 = 付け先の無いワールドの
        // アセットから鳴らす BGM など）。名前が無ければ付けた相手の音源だけ。
        if (name.empty() && (object < 0 || a.body != object)) continue;
        if (!name.empty() && object >= 0 && a.body != object && a.body >= 0) {
            // 名前が合っても別の物に付いた音源は鳴らさない（同じ名前の音源を物ごとに
            // 持たせて、触れた物の音だけ鳴らす使い方のため）。ワールドの音源は鳴らす。
            continue;
        }
        if (play) audioCountPlayLocked(audioRt_[i], simTime);
        else ++audioRt_[i].stop;
    }
}

void Scene::setAudioOverride(int source, double volume, double pitch) {
    if (source < 0) return;
    const std::uint64_t gen = runGeneration();
    const std::size_t n = editor_.audioSources().size();
    if (std::size_t(source) >= n) return;
    std::lock_guard<std::mutex> lk(audioMutex_);
    audioSyncLocked(gen, n);
    AudioRuntime& r = audioRt_[std::size_t(source)];
    r.volume = volume;
    r.pitch = pitch;
}

int Scene::audioSourceIndex(const std::string& name) {
    if (name.empty()) return -1;
    const auto sources = editor_.audioSources();
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].name == name) return int(i);
    }
    return -1;
}

// ---- 描いたフレームの音の状態（RENDER スレッド）-------------------------------------

void Scene::buildAudioFrame(std::size_t cameraIndex, wizengine::audio::AudioFrame& out) {
    namespace au = wizengine::audio;
    out.simulating = editor_.mode() == ed::AppMode::Simulate;
    out.paused = false;  // 一時停止は main が足す（PhysicsTuning は Scene の外）
    out.simTime = renderSimTime_;
    out.timeScale = timeScale_.load();
    out.version = editor_.audioVersion();
    out.sources.clear();

    // 付け先が生きているか（RENDER スレッドなので構造ロックを取る。ロックの順は
    // objects → editor → poses → audio）。位置はこのフレームに描いた姿勢
    // （renderPoses_ = 補間済み、RENDER 専用）= 映像に映っている位置。
    std::vector<char> alive;
    {
        auto lk = lockObjects();
        alive.resize(boxes_.size());
        for (std::size_t i = 0; i < boxes_.size(); ++i) alive[i] = boxes_[i].alive ? 1 : 0;
    }
    const auto sources = editor_.audioSources();
    const bool hasListener = editor_.hasAudioListener();
    const ed::AudioListenerDesc listener = editor_.audioListener();
    const std::uint64_t gen = runGeneration();
    out.generation = gen;
    const std::vector<BodyTransform>& poses = renderPoses_;
    auto bodyOk = [&](int b) {
        return b >= 0 && std::size_t(b) < alive.size() && alive[std::size_t(b)] != 0 &&
               std::size_t(b) < poses.size();
    };
    auto toV = [](const Vec3& v) { return au::AudioVec3{v.x(), v.y(), v.z()}; };

    // ---- 耳 ----
    au::AudioListenerState& L = out.listener;
    if (hasListener && bodyOk(listener.body)) {
        const BodyTransform& t = poses[std::size_t(listener.body)];
        const Quat q = quatOf(t) * scenemath::quatFromEulerDegrees(
                                       listener.rotation.x, listener.rotation.y, listener.rotation.z);
        L.pos = toV(worldPoint(poses, listener.body, listener.position, true));
        L.fwd = toV(q * Vec3(0.0, 0.0, -1.0));  // 前 = ローカル -Z（車両と同じ約束）
        L.up = toV(q * Vec3(0.0, 1.0, 0.0));
        L.attached = listener.body;
    } else {
        // そのページのカメラ（イベントのカメラ注文を当てた後 = このフレームに描いた視点）。
        const std::size_t cam = cameraIndex < cameras_.size() ? cameraIndex : 0;
        const scenemath::Basis b = scenemath::cameraBasis(*cameras_[cam]);
        L.pos = toV(b.eye);
        if (b.valid) {
            L.fwd = toV(b.forward);
            L.up = toV(b.up);
        }
        L.attached = -1;
    }
    L.volume = listener.volume;
    L.doppler = listener.doppler;
    L.soundSpeed = listener.soundSpeed;

    // ---- 音源 ----
    std::vector<AudioRuntime> rt;
    {
        std::lock_guard<std::mutex> lk(audioMutex_);
        audioSyncLocked(gen, sources.size());
        rt = audioRt_;
    }
    out.sources.reserve(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const ed::AudioSourceDesc& a = sources[i];
        const bool ok = bodyOk(a.body);
        au::AudioSourceState s;
        s.name = a.name;
        s.clip = a.clip;
        s.alive = a.body < 0 || ok;  // 付け先が消えた音源（番号は詰めない）は鳴らさない
        s.pos = toV(worldPoint(poses, a.body, a.position, ok));
        const AudioRuntime r = i < rt.size() ? rt[i] : AudioRuntime{};
        s.volume = (out.simulating && std::isfinite(r.volume)) ? r.volume : a.volume;
        s.pitch = (out.simulating && std::isfinite(r.pitch)) ? r.pitch : a.pitch;
        s.loop = a.loop;
        s.playOnAwake = a.playOnAwake;
        s.mute = a.mute;
        s.spatialBlend = a.spatialBlend;
        s.panStereo = a.panStereo;
        s.minDistance = a.minDistance;
        s.maxDistance = a.maxDistance;
        s.linearRolloff = a.rolloff == ed::AudioRolloff::Linear;
        s.dopplerLevel = a.dopplerLevel;
        s.play = r.play;
        s.stop = r.stop;
        s.playTimes = r.playTimes;
        out.sources.push_back(std::move(s));
    }
}

// ---- /scene の audio ----------------------------------------------------------------

nlohmann::json Scene::audioSceneJson() {
    nlohmann::json j;
    nlohmann::json arr = nlohmann::json::array();
    const auto sources = editor_.audioSources();
    for (std::size_t i = 0; i < sources.size(); ++i) {
        nlohmann::json e = ed::toJson(sources[i]);
        e["index"] = int(i);
        arr.push_back(std::move(e));
    }
    j["sources"] = std::move(arr);
    j["hasListener"] = editor_.hasAudioListener();
    j["listener"] = ed::toJson(editor_.audioListener());
    // assets/ の音のファイル（5 秒ごとに取り直す）。
    {
        std::lock_guard<std::mutex> lk(audioClipsMutex_);
        const double now = nowSeconds();
        if (now - audioClipsAt_ > 5.0) {
            audioClips_ = listAudioFiles();
            audioClipsAt_ = now;
        }
        j["clips"] = audioClips_;
    }
    return j;
}

// ---- 編集 ---------------------------------------------------------------------------

void Scene::applyAudioOp(const std::string& kind, const nlohmann::json& a) {
    auto sources = editor_.audioSources();
    if (kind == "audio.add") {
        if (sources.size() >= ed::kMaxAudioSources) {
            editor_.setStatus("オーディオソースが多すぎます（" +
                              std::to_string(ed::kMaxAudioSources) + " 個まで）");
            return;
        }
        ed::AudioSourceDesc d;
        // オブジェクトに付ける音は 3D、ワールドの音（BGM）は 2D から始める（Unity は
        // 既定 2D だが、物に付けたのに距離で変わらないのは分かりにくい）。
        const int body = ed::jsonInt(a, "body", -1);
        d.spatialBlend = body >= 0 ? 1.0 : 0.0;
        d.minDistance = 1.0;
        d.maxDistance = body >= 0 ? 50.0 : 500.0;
        d = ed::audioSourceFromJson(a, d);
        if (d.body >= 0 && !objectAlive(std::size_t(d.body))) d.body = -1;
        // 名前は一意に（制御器とノードが名前で引く）。
        std::string base = ed::sanitizeAudioName(d.name);
        if (base.empty()) base = "sound";
        auto taken = [&](const std::string& n) {
            for (const auto& s : sources) {
                if (s.name == n) return true;
            }
            return false;
        };
        std::string name = base;
        for (int k = 2; taken(name) && k < 1000; ++k) name = base + "_" + std::to_string(k);
        d.name = name;
        sources.push_back(ed::clampAudioSource(d));
        editor_.setAudioSources(std::move(sources));
        editor_.setStatus("オーディオソース「" + name + "」を追加しました");
        return;
    }
    const int index = ed::jsonInt(a, "index", -1);
    if (kind == "audio.remove") {
        if (index < 0 || std::size_t(index) >= sources.size()) return;
        const std::string name = sources[std::size_t(index)].name;
        sources.erase(sources.begin() + index);
        editor_.setAudioSources(std::move(sources));
        editor_.setStatus("オーディオソース「" + name + "」を削除しました");
        return;
    }
    if (kind == "audio.set") {
        if (index < 0 || std::size_t(index) >= sources.size()) return;
        ed::AudioSourceDesc d = ed::audioSourceFromJson(a, sources[std::size_t(index)]);
        if (d.body >= 0 && !objectAlive(std::size_t(d.body))) d.body = -1;
        // 名前を変えるなら、他の音源と重ならないように（重なったら元の名前のまま）。
        d.name = ed::sanitizeAudioName(d.name);
        for (std::size_t i = 0; i < sources.size(); ++i) {
            if (i != std::size_t(index) && !d.name.empty() && sources[i].name == d.name) {
                editor_.setStatus("名前「" + d.name + "」は他のオーディオソースが使っています");
                d.name = sources[std::size_t(index)].name;
                break;
            }
        }
        if (a.contains("clip") && a["clip"].is_string() && !a["clip"].get<std::string>().empty() &&
            !ed::audioClipAllowed(a["clip"].get<std::string>())) {
            editor_.setStatus("クリップは assets/ からの相対パスの音のファイル（.wav .ogg .mp3 など）です");
        }
        sources[std::size_t(index)] = ed::clampAudioSource(d);
        editor_.setAudioSources(std::move(sources));
        return;
    }
    if (kind == "audiolistener") {
        // "present": false = 耳の節を外す（= 各ページのカメラが耳）。
        if (a.contains("present") && a["present"].is_boolean() && !a["present"].get<bool>()) {
            editor_.setAudioListener(false, editor_.audioListener());
            editor_.setStatus("オーディオリスナーをカメラに戻しました");
            return;
        }
        ed::AudioListenerDesc l = ed::audioListenerFromJson(a, editor_.audioListener());
        if (l.body >= 0 && !objectAlive(std::size_t(l.body))) l.body = -1;
        editor_.setAudioListener(true, l);
        return;
    }
}
