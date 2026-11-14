#include <algorithm>
#include <cctype>
#include <optional>
#include <atomic>
#include <chrono>
#include <fstream>

#ifdef _WIN32
#include <direct.h>   // _getcwd, for the "where did we look?" message
#else
#include <unistd.h>
#endif
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>

#include <functional>
#include <memory>

#include <nlohmann/json.hpp>

#include <math/mat4.h>
#include <math/vec3.h>

#include "streaming/HttpServer.h"
#include "core/Log.h"
#include "core/Versions.h"
#include "core/AssetError.h"
#include "core/CpuAffinity.h"
#include "components/EditorComponent.h"
#include "document/EditorTypes.h"
#include "core/PortScan.h"
#include "components/PhysicsControlComponent.h"
#include "components/StreamControlComponent.h"
#include "components/TerrainComponent.h"
#include "components/ControlComponent.h"
#include "components/VehicleComponent.h"
#include "components/PrefabComponent.h"
#include "components/ScatterComponent.h"
#include "components/AutoGenComponent.h"
#include "app/AppConfig.h"
#include "autogen/AiConfig.h"
#include "autogen/HttpClient.h"
#include "physics/PhysicsTuning.h"
#include "physics/PhysicsWorld.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "core/Stats.h"
#include "streaming/VideoStreamer.h"
#include "streaming/WebRtcStreamer.h"
#include "scene/MathBridge.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
volatile std::sig_atomic_t g_run = 1;
void onSignal(int) { g_run = 0; }

#ifdef _WIN32
// Windows: std::signal(SIGINT) rides on the console control handler installed
// by the CRT, and whether the process survives after the handler returns is
// not guaranteed - in practice Ctrl-C could kill the process before the clean
// shutdown path (thread joins, pipeline teardown, "stopped.") ever ran.
// Registering our own handler and returning TRUE marks the event as fully
// handled, so nothing terminates the process behind our back.
BOOL WINAPI onConsoleCtrl(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:  // window close: ~5 s grace to shut down
            g_run = 0;
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

void installStopHandler() {
#ifdef _WIN32
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
#else
    std::signal(SIGINT, onSignal);
#endif
}
}  // namespace

// The real work; main() wraps it so a thrown exception is reported instead of
// terminating the process silently - which is what "it just closes" looks like
// when the .exe is started from Explorer and the console vanishes with it.
static int run(int argc, char** argv);

int main(int argc, char** argv) {
    wizengine::logging::init();
    wizengine::logging::setThreadName("main");
    try {
        return run(argc, argv);
    } catch (const wizengine::AssetError& e) {
        // Something the scene names (a material, a model, a texture) could
        // not be loaded. Stopping is
        // deliberate: running on with a missing model or texture produces a
        // scene that silently differs from the configured one.
        LOGE("app", "%s", e.what());
        std::printf(
            "\nCheck the file name (assets/scenes/*.xml or SceneConfig.h) and "
            "that the file sits under assets/ next to wizengine.exe.\n"
            "(press Enter to close)\n");
        std::getchar();
        return 1;
    } catch (const std::exception& e) {
        LOGE("app", "%s", e.what());
        std::printf("(press Enter to close)\n");
        std::getchar();
        return 1;
    } catch (...) {
        LOGE("app", "unknown failure during startup");
        std::printf("(press Enter to close)\n");
        std::getchar();
        return 1;
    }
}

// ---- 起動設定（config.json）------------------------------------------------
// 実行環境・システムに関わる設定（出力モードとポート・映像・CPU コア・ログ・
// カメラのスロット・起動シーン・物理バックエンドの選び方・AI の接続先）は全部
// config.json（app/AppConfig.h）。exe 引数は従来どおり使え、config.json より
// 優先される。
//
// Codec guide: H264 = the compatibility baseline (every browser and device,
// hardware encode). H265 = better quality per bit, viewers Chrome 136+/
// Safari 18+ only (Edge and Firefox never offer it in WebRTC). AV1 = best
// compression, royalty free, Chrome/Edge/Firefox viewers, needs an
// AV1-capable GPU (RDNA3+/RTX40+/Arc) and rtpav1pay. VP9 = the software
// (CPU-encode) fallback. H265/AV1 fall back to H264 with a warning when
// their pieces are missing.
static std::optional<VideoCodec> codecFromName(std::string v) {
    for (auto& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (v == "h264") return VideoCodec::H264;
    if (v == "h265" || v == "hevc") return VideoCodec::H265;
    if (v == "av1") return VideoCodec::AV1;
    if (v == "vp9") return VideoCodec::VP9;
    return std::nullopt;
}

static void printUsage() {
    std::printf(
        "usage: wizengine [web [httpPort] | window | stream [host] [port] | "
        "rtsp [url]]\n"
        "                 [--config FILE] [--physics-cores SPEC] "
        "[--render-cores SPEC]\n"
        "                 [--physics-threads N] [--max-cameras N]\n"
        "                 [--codec CODEC] [--encoder ELEMENT]\n\n"
        "All settings live in config.json (next to assets/; --config picks "
        "another file).\nThe options above override it for one run.\n\n"
        "SPEC is a core list: \"0-11\", \"0,2,4\", \"3\" or \"0-3,8,12-13\".\n"
        "CODEC is h264 (default; every browser), h265 (Chrome/Safari only),\n"
        "av1 (needs an AV1-capable GPU) or vp9 (software).\n"
        "ELEMENT is a GStreamer encoder element for that codec (amfh264enc = "
        "primary GPU,\namfh264device1enc = next, ...; list them with "
        "gst-inspect-1.0 amfcodec).\n");
}

int run(int argc, char** argv) {
    // ---- 1. config.json の場所（--config）と --help だけ先に見る -------------
    std::string configPath = "config.json";
    bool configExplicit = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h" || arg == "/?") {
            printUsage();
            return 0;
        }
        if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
            configExplicit = true;
        }
    }

    // ---- 2. config.json を読む ----------------------------------------------
    wizengine::AppConfig cfg;
    {
        const wizengine::ConfigLoad load = wizengine::loadAppConfig(configPath, cfg);
        if (!load.found) {
            if (configExplicit) {
                throw std::runtime_error("config file not found: " + configPath);
            }
            LOGI("app", "config.json not found in the working directory - using "
                        "built-in defaults");
        } else if (!load.error.empty()) {
            // 壊れた設定で黙って既定値のまま動くと、どの値が効いているのか
            // 分からなくなるので止める（main が理由を出して Enter を待つ）。
            throw std::runtime_error(configPath + ": " + load.error);
        } else {
            LOGI("app", "config: %s", configPath.c_str());
        }
        for (const auto& w : load.warnings) {
            LOGW("app", "config: %s", w.c_str());
        }
    }
    // ログの段階: 環境変数 WIZENGINE_LOG があればそちら（logging::init が読んだ
    // まま）、無ければ config.json の log.level。
    if (!std::getenv("WIZENGINE_LOG")) {
        using wizengine::logging::Level;
        const std::string& lv = cfg.logLevel;
        wizengine::logging::setLevel(lv == "debug"  ? Level::Debug
                                     : lv == "warn" ? Level::Warn
                                     : lv == "error" ? Level::Error
                                                     : Level::Info);
    }
    // API キーは config.json が空なら環境変数から。
    wizengine::autogen::fillAiKeysFromEnvironment(cfg.ai);

    // ---- 3. exe 引数で上書き（従来の起動スクリプトがそのまま効く）------------
    //   wizengine web [httpPort]         -> browser only (WebRTC video)
    //   wizengine window                 -> local preview window on this PC
    //   wizengine stream [host] [port]   -> RTP/UDP stream
    //   wizengine rtsp [url]             -> publish to an RTSP server
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 < argc) return argv[++i];
            LOGW("app", "%s needs a value - ignoring", what);
            return {};
        };
        if (arg == "--config") {
            next("--config");  // 1. で読んだ
        } else if (arg == "--physics-cores") {
            cfg.cpu.physicsCores = next("--physics-cores");
        } else if (arg == "--render-cores") {
            cfg.cpu.renderCores = next("--render-cores");
        } else if (arg == "--physics-threads") {
            const std::string v = next("--physics-threads");
            // windows.h の max マクロと衝突するので std::max は使わない。
            const int n = v.empty() ? cfg.cpu.physicsThreads : std::atoi(v.c_str());
            cfg.cpu.physicsThreads = n < 0 ? 0 : n;
        } else if (arg == "--max-cameras") {
            const std::string v = next("--max-cameras");
            const int n = v.empty() ? 0 : std::atoi(v.c_str());
            if (n >= 1 && n <= 16) {
                cfg.scene.maxCameras = std::size_t(n);
            } else if (!v.empty()) {
                LOGW("app", "--max-cameras %s is out of range (1-16) - ignoring",
                     v.c_str());
            }
        } else if (arg == "--encoder") {
            const std::string v = next("--encoder");
            if (!v.empty()) cfg.video.encoder = v;
        } else if (arg == "--codec") {
            const std::string v = next("--codec");
            if (codecFromName(v)) {
                cfg.video.codec = v;
            } else if (!v.empty()) {
                LOGW("app", "--codec %s is not one of h264/h265/av1/vp9 - ignoring",
                     v.c_str());
            }
        } else {
            positional.push_back(arg);
        }
    }
    if (!positional.empty()) {
        cfg.server.mode = positional[0];
        if (cfg.server.mode == "stream") {
            if (positional.size() > 1) cfg.server.streamHost = positional[1];
            if (positional.size() > 2) cfg.server.streamPort = std::atoi(positional[2].c_str());
        } else if (cfg.server.mode == "rtsp") {
            if (positional.size() > 1) cfg.server.rtspUrl = positional[1];
        } else if (cfg.server.mode == "web" && positional.size() > 1) {
            const int p = std::atoi(positional[1].c_str());
            if (p >= 1 && p <= 65535) {
                cfg.server.httpPort = p;
            } else {
                LOGW("http", "port '%s' is out of range - using %d",
                     positional[1].c_str(), cfg.server.httpPort);
            }
        } else if (cfg.server.mode != "web" && cfg.server.mode != "window") {
            LOGW("app", "unknown mode '%s' - using web", cfg.server.mode.c_str());
            cfg.server.mode = "web";
        }
    }
    if (cfg.scene.editorCamera >= cfg.scene.maxCameras) cfg.scene.editorCamera = 0;
    if (!cfg.video.encoder.empty()) WebRtcStreamer::setEncoderOverride(cfg.video.encoder);

    // 以下は確定した設定の写し（従来の変数名のまま使う）。
    const int kWidth = cfg.video.width;
    const int kHeight = cfg.video.height;
    const int kFps = cfg.video.fps;
    const VideoCodec videoCodec = codecFromName(cfg.video.codec).value_or(VideoCodec::H264);
    const std::string& physicsCoreSpec = cfg.cpu.physicsCores;
    const std::string& renderCoreSpec = cfg.cpu.renderCores;
    const int physicsThreads = cfg.cpu.physicsThreads;
    OutputMode outputMode = OutputMode::None;
    const std::string& host = cfg.server.streamHost;
    const int port = cfg.server.streamPort;
    const int httpPort = cfg.server.httpPort;
    const std::string& rtspUrl = cfg.server.rtspUrl;
    if (cfg.server.mode == "window") outputMode = OutputMode::Window;
    else if (cfg.server.mode == "stream") outputMode = OutputMode::Stream;
    else if (cfg.server.mode == "rtsp") outputMode = OutputMode::Rtsp;
    const std::string materialPath = "shaded.filamat";

    // Everything below is loaded from the working directory, so running the
    // .exe from somewhere else (double-clicking it, or a shortcut with the
    // wrong "start in") used to fail deep inside Filament with no explanation.
    // Check up front and say exactly what is missing and where we looked.
    {
        struct Required {
            const char* path;
            const char* what;
            bool fatal;
        };
        // Only files that NO loader checks for itself. The materials and any
        // model or texture are validated where they are read (AssetError), so
        // listing them here as well would be a second list to keep in sync -
        // the very thing that let a newly added file slip through before.
        //
        // The browser UI is the exception: it is served per request, so a
        // missing index.html would only show up as a blank page much later.
        const Required required[] = {
            {"assets/web/index.html", "browser UI", true},
        };

        std::vector<std::string> missing, optional;
        for (const auto& r : required) {
            std::ifstream f(r.path, std::ios::binary);
            if (f) continue;
            (r.fatal ? missing : optional)
                .push_back(std::string(r.path) + "  (" + r.what + ")");
        }

        if (!optional.empty()) {
            LOGW("app", "optional files not found:");
            for (const auto& m : optional) LOGW("app", "  %s", m.c_str());
        }
        if (!missing.empty()) {
            char cwd[1024] = {0};
#ifdef _WIN32
            _getcwd(cwd, sizeof(cwd));
#else
            getcwd(cwd, sizeof(cwd));
#endif
            LOGE("app", "required files are missing (current directory: %s)",
                 cwd);
            for (const auto& m : missing) LOGE("app", "  missing: %s", m.c_str());
            std::printf(
                "\nThese are produced by the build and live next to the "
                "executable.\n"
                "Run wizengine.exe from that folder, for example:\n"
                "  cd build\\Release  (or wherever wizengine.exe is)\n"
                "  wizengine.exe\n"
                "If you started it by double-clicking, set the shortcut's "
                "\"Start in\" to that folder.\n\n(press Enter to close)\n");
            std::getchar();
            return 1;
        }
    }

    installStopHandler();

    // Backend (core / multicore): config.json の physics.backend。
    const PhysicsBackend requested = cfg.scene.backend;
    PhysicsWorld physics(requested);
    if (wizengine::engineCodename()[0] != '\0') {
        LOGI("app", "%s %s \"%s\"", wizengine::productName(), wizengine::engineVersion(),
             wizengine::engineCodename());
    } else {
        LOGI("app", "%s %s", wizengine::productName(), wizengine::engineVersion());
    }
    LOGI("physics",
         "backend: requested=%s  compiled-in multicore=%s  using=%s",
         requested == PhysicsBackend::Multicore ? "multicore" : "core",
         multicoreAvailable() ? "yes" : "no", physics.backendName());
    wizengine::Renderer renderer(kWidth, kHeight, materialPath);
    // カメラのスロット・起動シーンとモード・休止・バックエンドの選び方
    // （config.json の scene / physics）。
    Scene scene(physics, renderer, cfg.scene);
    // ---- CPU cores -------------------------------------------------------
    // Pin before anything starts: Chrono sizes its worker pool at build time,
    // and those workers inherit the affinity of the thread that creates them.
    const wizengine::CoreSet physicsCores =
        wizengine::parseCoreSet(physicsCoreSpec);
    const wizengine::CoreSet renderCores =
        wizengine::parseCoreSet(renderCoreSpec);
    {
        const wizengine::CoreSet available = wizengine::availableCores();
        LOGI("cpu", "this process may use %s", available.describe().c_str());
        if (!physicsCores.empty()) {
            LOGI("cpu", "physics on %s", physicsCores.describe().c_str());
        }
        if (!renderCores.empty()) {
            LOGI("cpu", "render/encode on %s",
                 renderCores.describe().c_str());
        }
        int threads = physicsThreads;
        if (threads < 1 && !physicsCores.empty()) {
            threads = physicsCores.count();  // one worker per pinned core
        }
        if (threads >= 1) physics.setNumThreads(threads);
    }

    scene.build();
    // build() の中でシーン設定がエディタ文書の初期値になるので、レートや
    // モードを読むのはここから。
    LOGI("physics", "rate: %d Hz (scene default), substeps %d",
         scene.physicsHz(), scene.substeps());
    LOGI("app", "start mode: %s (switchable from the browser header)",
         wizengine::editor::modeName(scene.mode()));

    // This thread runs the render loop; keep it off the physics cores.
    if (!renderCores.empty() && !wizengine::pinCurrentThread(renderCores)) {
        LOGW("cpu", "could not pin the render thread");
    }

    // Render pacing (kFps) and the physics timestep are independent.
    const double dt = 1.0 / kFps;
    const auto frameStep = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(dt));
    auto next = std::chrono::steady_clock::now();

    // One endpoint per camera: camera i is served on httpPort + i, with its
    // own page, its own WebRTC stream and its own viewer session. They all
    // drive the same scene, so anyone can grab and push objects.

    // 「まだ動画ビューが無い」印。予備スロットのビュー（スワップチェーン +
    // 読み戻しバッファ）は起動時には作らず、エディタで「カメラを追加」した
    // 時点（またはページに視聴者が来た時点）で描画スレッドが生成する。
    constexpr std::size_t kNoView = std::size_t(-1);

    struct CameraEndpoint {
        std::unique_ptr<HttpServer> http;
        std::unique_ptr<WebRtcStreamer> webrtc;
        std::function<void(const uint8_t*, size_t)> pushFrame;
        std::size_t viewIndex = 0;
        std::string path;  // "/cam0/", "/cam1/", ... on the shared listener

        // Stream format requested from the browser (StreamControlComponent,
        // INPUT thread) and applied by the render loop at a safe point.
        std::atomic<bool> formatDirty{false};
        std::atomic<int> reqW{0}, reqH{0}, reqFps{0}, reqKbps{0};
        // Per-camera frame pacing: frames are pushed at this rate even though
        // the loop runs at the global kFps.
        int fps = 0;
        std::chrono::steady_clock::time_point nextFrameDue{};
    };
    std::vector<std::unique_ptr<CameraEndpoint>> endpoints;
    std::unique_ptr<HttpListener> listener;  // the one port they all share

    // web mode -> WebRTC to the browser (no GStreamer sink). Other modes -> the
    // GStreamer VideoStreamer, which stays single-camera (camera 0).
    std::unique_ptr<VideoStreamer> streamer;
    std::function<void(const uint8_t*, size_t)> pushFrame;

    // Live-tunable physics knobs, driven from any camera's browser page.
    // The commands land in PhysicsControlComponent (via dispatchCommand);
    // the physics loop below applies them - main owns the state because it
    // owns the loop.
    PhysicsTuning tuning;
    tuning.substeps.store(scene.substeps());
    tuning.physicsHz.store(scene.physicsHz());
    tuning.envelope.store(scene.collisionEnvelope());
    tuning.recovery.store(scene.contactRecovery());
    scene.addComponent(std::make_unique<PhysicsControlComponent>(tuning));
    // テレインの筆（edit.terrain.brush）とカーソルの輪。EditorComponent より
    // 前に登録する: あちらは edit.* を全部受け止める（知らない種類も消費する）ため。
    scene.addComponent(std::make_unique<TerrainComponent>());
    // エディタモードの操作（配置・ジョイント・シーンの保存/読込）。物理の
    // レート系だけは PhysicsTuning を共有するので、それを渡しておく。
    scene.addComponent(std::make_unique<EditorComponent>(tuning));
    // 車両（<vehicle> を持つオブジェクト）。ブラウザの WASD を受け、
    // シミュレート中にサスとタイヤの力を車体へ掛ける。/stats へ計測値を
    // 出すために生ポインタも持っておく（所有は Scene）。
    auto vehicleOwner = std::make_unique<VehicleComponent>();
    VehicleComponent* vehicleComp = vehicleOwner.get();
    // 制御器（計算式をブロック線図として回す）。車両より先に登録する: 物理
    // ステップごとに運転入力の上書きを書き、車両がそのステップで使うため。
    // /scene と /control へ出すために生ポインタも持つ（所有は Scene）。
    auto controlOwner = std::make_unique<ControlComponent>(vehicleComp);
    ControlComponent* controlComp = controlOwner.get();
    scene.addComponent(std::move(controlOwner));
    scene.addComponent(std::move(vehicleOwner));
    // プレハブ（見た目の部品）の描画。車両の車輪姿勢は VehicleComponent から。
    scene.addComponent(std::make_unique<PrefabComponent>(vehicleComp));
    // テレインの散布物（木や小物）の描画。
    scene.addComponent(std::make_unique<ScatterComponent>());
    // AI のシーン自動構築（ai.generate / ai.cancel）。/scene に状態を出す
    // ため生ポインタも持つ（所有は Scene）。
    auto aiOwner = std::make_unique<AutoGenComponent>(cfg.ai);
    AutoGenComponent* aiComp = aiOwner.get();
    scene.addComponent(std::move(aiOwner));
    if (!wizengine::net::httpsSupported()) {
        LOGI("ai", "this build has no OpenSSL - only http:// AI endpoints can be used");
    }

    PerfStats stats;
    stats.substeps.store(scene.substeps());
    stats.iterations.store(scene.solverIterations());

    if (outputMode == OutputMode::None) {
        // One port for everything: cameras live under path prefixes on a
        // single shared listener. A port already in use (another instance, an
        // unrelated program) is skipped instead of failing to bind.
        listener = std::make_unique<HttpListener>(
            wizengine::findFreePortRun(httpPort, 1));
        for (std::size_t i = 0; i < scene.cameraCount(); ++i) {
            auto ep = std::make_unique<CameraEndpoint>();
            ep->path = "/cam" + std::to_string(i) + "/";
            // Camera 0 uses the view the renderer already created. 他の有効な
            // カメラはここで作り、無効（予備）スロットは kNoView のまま =
            // エディタで追加されるかページが訪問された時点で、描画ループが
            // フレーム境界で生成する（下の「動画ビューの遅延生成」）。
            // まだスレッドは動いていないので cameraActive はロック無しで
            // 読んでよい。
            if (i == 0) {
                ep->viewIndex = 0;
            } else if (scene.cameraActive(i)) {
                ep->viewIndex = renderer.addView();
            } else {
                ep->viewIndex = kNoView;
            }

            WebRtcStreamer::setPreferGpuConvert(cfg.video.gpuConvert);
            ep->webrtc = std::make_unique<WebRtcStreamer>(
                kWidth, kHeight, kFps, videoCodec, cfg.video.bitrate);
            ep->http = std::make_unique<HttpServer>("assets/web");
            listener->mount(*ep->http, "/cam" + std::to_string(i));

            WebRtcStreamer* rtc = ep->webrtc.get();
            ep->http->setOfferHandler(
                [rtc](const std::string& offer) { return rtc->handleOffer(offer); });
            ep->http->setViewerGoneHandler([rtc] { rtc->stopSession(); });
            ep->pushFrame = [rtc](const uint8_t* d, size_t sz) {
                rtc->pushFrame(d, sz);
            };

            const std::size_t camIndex = i;
            ep->http->setStatsProvider([&, camIndex] {
                nlohmann::json j;
                j["physicsHz"] = stats.physicsHz.load();
                j["physicsMs"] = stats.physicsMs.load();
                j["substeps"] = stats.substeps.load();
                j["iterations"] = stats.iterations.load();
                j["bodies"] = stats.bodies.load();
                j["asleep"] = stats.asleep.load();
                j["solverMs"] = stats.solverMs.load();
                j["collisionMs"] = stats.collisionMs.load();
                j["renderFps"] = stats.renderFps.load();
                j["renderMs"] = stats.renderMs.load();
                j["frameMs"] = stats.frameMs.load();
                j["targetFps"] = kFps;
                j["physicsTarget"] = tuning.physicsHz.load();
                j["envelope"] = tuning.envelope.load();
                j["recovery"] = tuning.recovery.load();
                j["realtime"] = stats.realtime.load();
                j["simTime"] = stats.simTime.load();
                j["paused"] = tuning.paused.load();
                // 車両の計測値（車両があるときだけ）。オーバーレイの
                // 「car」欄がこれを出す。
                if (const auto v = vehicleComp->snapshot(); v.present) {
                    j["vehicle"] = {{"rpm", v.rpm},     {"gear", v.gear},
                                    {"speed", v.speed}, {"clutch", v.clutch},
                                    {"count", v.count}};
                }
                // エディタ / シミュレートのどちらで動いているか。ブラウザの
                // 再生ボタンとタブの見た目がこれで決まる。
                j["mode"] = wizengine::editor::modeName(scene.mode());
                j["joints"] = int(scene.editor().jointCount());
                j["streamW"] = endpoints[camIndex]->webrtc->width();
                j["streamH"] = endpoints[camIndex]->webrtc->height();
                j["streamFps"] = endpoints[camIndex]->webrtc->fps();
                j["streamKbps"] =
                    endpoints[camIndex]->webrtc->bitrateBps() / 1000;
                j["versions"] = wizengine::versionsJson();
                j["engine"] = physics.backendName();
                j["contact"] = physics.contactName();
                j["engineNote"] = scene.backendNote();
                j["codec"] = endpoints[camIndex]->webrtc->codecName();
                j["camera"] = int(camIndex);
                j["cameraCount"] = int(scene.cameraCount());
                // どのカメラがエディタか（ヘッダのラベルが名前で呼ぶため）。
                j["editorCam"] = int(scene.editorCamera());
                return j.dump();
            });

            // Hierarchy for the sidebar: cameras and objects, with this
            // page's own camera marked so it can show its selection.
            ep->http->setSceneProvider([&, camIndex] {
                // The page is told each camera's path on the shared port,
                // so switching cameras is a plain navigation.
                nlohmann::json j = nlohmann::json::parse(
                    scene.hierarchyJson(camIndex), nullptr, false);
                if (j.is_discarded()) return scene.hierarchyJson(camIndex);
                for (auto& e : endpoints) {
                    j["paths"].push_back(e->path);
                    // Whether someone is watching that camera right now, so the
                    // sidebar can show which ones are taken - only one browser
                    // at a time can view a given camera.
                    j["busy"].push_back(e->http->hasViewer());
                }
                // AI のシーン自動構築の状態（🤖 AI モーダルが描く）。
                j["ai"] = aiComp->statusJson();
                // 制御器の実行状態（コンパイルの成否・警告・評価回数）。
                j["control"] = controlComp->statusJson();
                return j.dump();
            });
            // 制御器の記録（スコープの時系列と CSV）。
            ep->http->setControlProviders(
                [controlComp](std::uint64_t since) { return controlComp->samplesJson(since); },
                [controlComp](const std::string& formula) {
                    return controlComp->samplesCsv(formula);
                });

            // シーン文書そのもの（保存されるのと同じ XML）。ブラウザの
            // アセットパネルの「📄 XML」から開く。
            ep->http->setSceneXmlProvider([&scene] { return scene.documentXml(); });

            endpoints.push_back(std::move(ep));
        }
        // Browser "stream" commands land here (INPUT thread): store, mark
        // dirty - the render loop applies at a frame boundary.
        scene.addComponent(std::make_unique<StreamControlComponent>(
            [&endpoints](std::size_t cam, int w, int h, int fps, int kbps) {
                if (cam >= endpoints.size()) return;
                auto& ep = *endpoints[cam];
                ep.reqW.store(w);
                ep.reqH.store(h);
                ep.reqFps.store(fps);
                ep.reqKbps.store(kbps);
                ep.formatDirty.store(true);
            }));

        // ギズモ（エディタ専用レイヤ）はエディタカメラのビューにだけ映す。
        // 他のビューは addView が「見せない」で作っている。
        renderer.setViewEditorLayerVisible(
            endpoints[scene.editorCamera()]->viewIndex, true);

        LOGI("video", "%s @ %.1f Mbps, %zu camera(s)",
             endpoints[0]->webrtc->codecName(),
             endpoints[0]->webrtc->bitrateBps() / 1e6, endpoints.size());
    } else {
        streamer = std::make_unique<VideoStreamer>(
            outputMode, kWidth, kHeight, kFps,
            (outputMode == OutputMode::Stream) ? host.c_str() : rtspUrl.c_str(),
            port);
        pushFrame = [&](const uint8_t* d, size_t sz) { streamer->pushFrame(d, sz); };
    }

    if (listener) listener->start();
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        // standby = エディタの「カメラを追加」用の予備スロット。ページは
        // あるが、動画ビューは追加（または初訪問）の時点で作られる。
        LOGI("http", "camera %zu%s: http://127.0.0.1:%d%s", i,
             i == scene.editorCamera()
                 ? " (editor)"
                 : (scene.cameraActive(i) ? "" : " (standby)"),
             listener->port(), endpoints[i]->path.c_str());
    }

    // ---- Physics thread --------------------------------------------------
    // Chrono runs here at a fixed timestep; it only writes pose snapshots.
    // False while nobody is watching: both threads go quiet (see below).
    std::atomic<bool> gActive{true};
    std::thread physicsThread([&] {
        wizengine::logging::setThreadName("physics");
        if (!physicsCores.empty()) {
            if (!wizengine::pinCurrentThread(physicsCores)) {
                LOGW("cpu", "could not pin the physics thread");
            }
            // Chrono::Multicore solves on OpenMP workers, which are separate
            // threads: pinning this one does not constrain them. Pin them from
            // here, on the thread that will drive the solver, before the first
            // step so the pool is already in place and bound.
            wizengine::pinOpenMpWorkers(
                physicsCores,
                physicsThreads > 0 ? physicsThreads : physicsCores.count());
        }
        // Wall-clock-driven fixed timestep. Simulated time follows real time:
        // each pass consumes the elapsed wall time and runs as many fixed dt
        // updates as it covers. When a burst is expensive we catch up over the
        // next passes instead of silently running in slow motion.
        //
        // If the machine simply cannot keep up, we cap the catch-up
        // (kMaxCatchUp) and drop the backlog - the sim then does run slower
        // than real time, which the "speed" figure in the overlay reports.
        constexpr int kMaxCatchUp = 4;

        auto prev = std::chrono::steady_clock::now();
        double accumulator = 0.0;

        auto window = prev;
        int updates = 0;
        double busyMs = 0.0;
        double solverMs = 0.0;
        double collisionMs = 0.0;
        double simAdvanced = 0.0;  // simulated seconds in this window
        double simTotal = 0.0;     // simulated seconds since start (see PerfStats)

        // Chrono が投げた例外（未実装の口・壊れた拘束など）はこのスレッドで
        // 捕まえないとプロセスごと落ちて、原因がコンソールにも残らない。
        // 捕まえたら理由をログとステータスに出してエディタへ戻す（拘束を外して
        // 設計状態へ）。それでも続けて投げるなら物理を止めて描画だけ続ける。
        int physicsFailures = 0;
        auto guarded = [&](const char* what, auto&& fn) {
            try {
                fn();
                return true;
            } catch (const std::exception& e) {
                ++physicsFailures;
                LOGE("physics", "exception in %s: %s", what, e.what());
                scene.editor().setStatus(std::string("物理で例外: ") + e.what());
            } catch (...) {
                ++physicsFailures;
                LOGE("physics", "unknown exception in %s", what);
                scene.editor().setStatus("物理で例外（詳細不明）");
            }
            if (physicsFailures < 3) {
                scene.editor().requestMode(wizengine::editor::AppMode::Editor);
            } else {
                LOGE("physics", "physics keeps throwing - stepping stopped (reload a "
                                "scene or restart)");
            }
            return false;
        };

        while (g_run) {
            if (!gActive.load()) {
                // Nobody is watching: no stepping at all, and no backlog to
                // catch up on when a viewer returns.
                accumulator = 0.0;
                prev = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (physicsFailures >= 3) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // 編集操作とモード切替はどちらのモードでも処理する（シミュレート
            // 中に設定を変えたり、物を足したりできるように）。Chrono を触る
            // のはこのスレッドだけ、という約束はここで守られる。
            if (!guarded("edit / mode switch", [&] { scene.applyPendingEdits(); })) continue;

            if (scene.mode() == wizengine::editor::AppMode::Editor) {
                // エディタモード: 物理は進めない。掴んだ物の置き直しと姿勢の
                // 更新だけを、描画と同じくらいの間隔で回す。
                accumulator = 0.0;
                prev = std::chrono::steady_clock::now();
                scene.stepEditor(1.0 / 60.0);
                stats.physicsHz.store(0.0);
                stats.physicsMs.store(0.0);
                stats.realtime.store(1.0);
                stats.bodies.store(int(physics.bodyCount()));
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - prev).count();
            prev = now;
            if (elapsed > 0.25) elapsed = 0.25;  // ignore huge stalls
            if (!tuning.paused.load()) accumulator += elapsed;

            if (const int iters = tuning.pendingIterations.exchange(0);
                iters > 0) {
                physics.setSolverIterations(iters);
                stats.iterations.store(iters);
            }
            if (const double rec = tuning.pendingRecovery.exchange(0.0);
                rec > 0.0) {
                // How fast existing overlap is pushed out (m/s).
                physics.setContactRecoverySpeed(rec);
                tuning.recovery.store(rec);
            }
            if (const double env = tuning.pendingEnvelope.exchange(0.0);
                env > 0.0) {
                // Speculative contacts: a bigger envelope starts resolving the
                // contact before the shapes actually overlap.
                physics.setCollisionTolerances(env, env);
                tuning.envelope.store(env);
            }
            // レートとサブステップはシーン設定（SimSettings）が正: System タブ
            // （PhysicsControlComponent）も Physics タブ（edit.sim）も同じ値を
            // ミラーしているし、シーンの読込（<option rate substeps>）はそこに
            // しか書かない。以前は起動時に 1 回だけ tuning へ写していたので、
            // rate="120" のシーンを読んでも 30 Hz のまま回っていた（ケーブルの
            // ような硬い系が暴れる原因）。tuning にも写して /stats の目標値を揃える。
            // windows.h の max マクロと衝突するので std::max は使わない。
            int substeps = scene.substeps();
            if (substeps < 1) substeps = 1;
            int hz = scene.physicsHz();
            if (hz < 1) hz = 1;
            tuning.substeps.store(substeps);
            tuning.physicsHz.store(hz);
            const double stepDt = 1.0 / hz;  // physics timestep, not the frame time
            const double subDt = stepDt / substeps;
            stats.substeps.store(substeps);

            if (tuning.reset.exchange(false)) {
                scene.reset();
                simTotal = 0.0;  // the sim clock restarts with the scene
                stats.simTime.store(0.0);
            }

            const auto t0 = std::chrono::steady_clock::now();
            int stepped = 0;
            bool stepOk = true;
            while (stepOk && accumulator >= stepDt && stepped < kMaxCatchUp) {
                stepOk = guarded("physics step", [&] {
                    for (int i = 0; i < substeps; ++i) {
                        scene.stepPhysics(subDt);
                        const StepTimers t = physics.timers();  // seconds
                        solverMs += t.solver * 1000.0;
                        collisionMs += t.collision * 1000.0;
                    }
                });
                if (!stepOk) {
                    accumulator = 0.0;
                    break;
                }
                accumulator -= stepDt;
                simAdvanced += stepDt;
                simTotal += stepDt;
                ++stepped;
            }
            if (accumulator >= stepDt) {
                accumulator = 0.0;  // behind and out of catch-up budget
            }
            // このパスの終わりの姿勢を描画の補間用に公開する（描画は前回と
            // 今回のパスの間を補間する = 物理 30 Hz でも 60 fps で滑らか）。
            if (stepped > 0) scene.endPhysicsPass();
            const auto t1 = std::chrono::steady_clock::now();

            if (stepped > 0) {
                stats.simTime.store(simTotal);
                busyMs += std::chrono::duration<double, std::milli>(t1 - t0).count() / stepped;
                updates += stepped;
            }

            // Publish averages twice a second.
            const double windowMs =
                std::chrono::duration<double, std::milli>(t1 - window).count();
            if (windowMs >= 500.0) {
                const double windowSec = windowMs / 1000.0;
                stats.physicsHz.store(updates / windowSec);
                stats.physicsMs.store(updates > 0 ? busyMs / updates : 0.0);
                stats.solverMs.store(updates > 0 ? solverMs / updates : 0.0);
                stats.collisionMs.store(updates > 0 ? collisionMs / updates : 0.0);
                stats.realtime.store(
                    tuning.paused.load() ? 1.0 : simAdvanced / windowSec);
                // Same thread that steps Chrono, so this read is safe.
                stats.asleep.store(static_cast<int>(physics.sleepingCount()));
                stats.bodies.store(static_cast<int>(physics.bodyCount()));
                window = t1;
                updates = 0;
                busyMs = 0.0;
                solverMs = 0.0;
                collisionMs = 0.0;
                simAdvanced = 0.0;
            }

            // Nothing to do until the next step is due; keep the thread from
            // spinning while still waking up promptly.
            if (accumulator < stepDt) {
                const double waitSec = stepDt - accumulator;
                std::this_thread::sleep_for(std::chrono::duration<double>(
                    waitSec > 0.002 ? waitSec * 0.5 : 0.0005));
            }
        }
    });

    // ---- Input thread ----------------------------------------------------
    // Browser commands are applied here rather than once per rendered frame:
    // at 1080p a frame can take long enough that key presses felt sluggish and
    // held arrow keys stacked up. Polling at 4 ms keeps input responsive
    // regardless of how heavy rendering gets.
    std::thread inputThread([&] {
        wizengine::logging::setThreadName("input");
        while (g_run) {
        for (std::size_t camIndex = 0; camIndex < endpoints.size(); ++camIndex) {
        for (const auto& body : endpoints[camIndex]->http->drainCommands()) {
            const auto msg = nlohmann::json::parse(body, nullptr, false);
            if (msg.is_discarded() || !msg.is_object()) continue;
            const auto it = msg.find("cmd");
            if (it == msg.end() || !it->is_string()) continue;

            // Every command is handled by whichever SceneComponent claims it
            // (camera, grabbing, picking - and engine tuning, which lives in
            // PhysicsControlComponent). Adding a command never touches this
            // file again.
            scene.dispatchCommand(camIndex, msg);
        }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    });

    auto statWindow = std::chrono::steady_clock::now();
    int frames = 0;
    double renderMs = 0.0;
    double loopMs = 0.0;

    // In web mode the engine only works while a browser holds the viewer
    // session. The other output modes (window/stream/rtsp) always run.
    const bool gateOnViewer =
        scene.idleWhenUnwatched() && outputMode == OutputMode::None;
    bool wasActive = true;

    while (g_run) {
        if (gateOnViewer) {
            bool active = false;
            for (auto& ep : endpoints) {
                if (ep->http->hasViewer()) active = true;  // check all: each
            }                                             // expires its own
            if (active != wasActive) {
                LOGI("app", "engine %s (viewer %s)",
                     active ? "resumed" : "idle",
                     active ? "connected" : "gone");
                wasActive = active;
                if (!active) {
                    stats.physicsHz.store(0.0);
                    stats.renderFps.store(0.0);
                    stats.realtime.store(0.0);
                }
            }
            gActive.store(active);
            if (!active) {
                // Nothing to render for. Poll for a viewer at a leisurely rate.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                next = std::chrono::steady_clock::now();
                continue;
            }
        }

        const auto loopStart = std::chrono::steady_clock::now();

        // ---- 動画ビューの遅延生成 -----------------------------------------
        // 予備スロットのビューは、エディタで「カメラを追加」されたか、その
        // ページに視聴者が来た時点で作る。Filament を触ってよいのはこの
        // スレッドだけなので、フレーム境界のここが安全な作成ポイント。
        // 一度作ったビューは返さない（カメラの削除は一覧から消すだけ。
        // 次の追加で同じスロットとビューを再利用する）。
        if (!endpoints.empty()) {
            std::vector<char> camActive(endpoints.size(), 0);
            {
                auto lk = scene.lockObjects();  // camerasActive_ は構造ロック下
                for (std::size_t i = 0; i < endpoints.size(); ++i) {
                    camActive[i] = scene.cameraActive(i) ? 1 : 0;
                }
            }
            for (std::size_t i = 0; i < endpoints.size(); ++i) {
                auto& ep = *endpoints[i];
                if (ep.viewIndex != kNoView) continue;
                if (!camActive[i] && !ep.http->hasViewer()) continue;
                ep.viewIndex = renderer.addView();
                LOGI("video", "camera %zu: video view created (%s)", i,
                     camActive[i] ? "added in editor" : "viewer arrived");
            }
        }

        // Copy the latest physics poses to the renderables.
        scene.applyToRenderer();

        // Each camera turns its state into eye/target for its own view.
        // applyToRenderer の後: イベントのカメラのアクション（注視・後ろへ・
        // 距離と仰角）はそこでこのフレームの姿勢からカメラを動かすので、先に
        // 当てると 1 フレーム古いカメラで描いて、追わせた車が画面の中で震える。
        for (std::size_t i = 0; i < endpoints.size(); ++i) {
            if (endpoints[i]->viewIndex == kNoView) continue;  // ビュー未生成
            scene.camera(i).applyTo(renderer, endpoints[i]->viewIndex);
        }
        if (endpoints.empty()) scene.camera(0).applyTo(renderer);
        const auto renderStart = std::chrono::steady_clock::now();
        if (endpoints.empty()) {
            renderer.renderFrame([&](const uint8_t* data, size_t size) {
                pushFrame(data, size);
            });
        } else {
            // One render + encode per camera. Only cameras with a viewer are
            // drawn, so idle pages cost nothing.
            const auto nowTp = std::chrono::steady_clock::now();
            for (auto& ep : endpoints) {
                // ビュー未生成のスロットは描画も配信も無い。formatDirty は
                // 消さずに残す（ビューができた最初のフレームで適用される）。
                if (ep->viewIndex == kNoView) continue;
                if (ep->formatDirty.exchange(false)) {
                    // Safe point: this thread owns the renderer. The view
                    // resize completes inside renderFrame once its readbacks
                    // drain; the streamer format applies on the browser's
                    // reconnect.
                    renderer.requestViewResize(ep->viewIndex, ep->reqW.load(),
                                               ep->reqH.load());
                    ep->webrtc->setStreamFormat(ep->reqW.load(),
                                                ep->reqH.load(),
                                                ep->reqFps.load(),
                                                ep->reqKbps.load() * 1000);
                    ep->fps = ep->reqFps.load();
                    ep->nextFrameDue = nowTp;
                }
                if (!ep->http->hasViewer()) continue;
                // Per-camera rate: skip the whole render+readback for this
                // view until its next frame is due (a 30 fps camera costs
                // half of a 60 fps one, not just half the encode).
                if (ep->fps > 0 && ep->fps < kFps) {
                    if (nowTp < ep->nextFrameDue) continue;
                    ep->nextFrameDue =
                        nowTp + std::chrono::microseconds(1000000 / ep->fps);
                }
                renderer.renderFrame(ep->viewIndex,
                                     [&](const uint8_t* data, size_t size) {
                                         ep->pushFrame(data, size);
                                     });
            }
        }
        const auto renderEnd = std::chrono::steady_clock::now();

        renderMs += std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
        loopMs += std::chrono::duration<double, std::milli>(renderEnd - loopStart).count();
        ++frames;
        const double statMs =
            std::chrono::duration<double, std::milli>(renderEnd - statWindow).count();
        if (statMs >= 500.0) {
            stats.renderFps.store(frames * 1000.0 / statMs);
            stats.renderMs.store(renderMs / frames);
            stats.frameMs.store(loopMs / frames);
            statWindow = renderEnd;
            frames = 0;
            renderMs = 0.0;
            loopMs = 0.0;
        }

        // Fixed-cadence pacing: fall back to "now" if we slipped a whole frame
        // so we never spin trying to catch up on a large stall.
        next += frameStep;
        const auto now = std::chrono::steady_clock::now();
        if (next < now) next = now;
        std::this_thread::sleep_until(next);
    }

    if (inputThread.joinable()) inputThread.join();
    if (physicsThread.joinable()) physicsThread.join();
    // AI の生成がまだ通信中でも、以後は結果を Scene へ渡さない（スレッドは
    // 切り離してあるので待たない）。
    aiComp->shutdown();
    // Readbacks are asynchronous, so let the in-flight ones land before the
    // encoders and their buffers go away.
    renderer.finishPendingReadbacks();
    if (listener) listener->stop();
    std::puts("\nstopped.");
    return 0;
}
