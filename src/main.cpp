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
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>

#include <functional>
#include <memory>

#include <nlohmann/json.hpp>

#include <math/mat4.h>
#include <math/vec3.h>

#include "HttpServer.h"
#include "Log.h"
#include "Versions.h"
#include "AssetError.h"
#include "CpuAffinity.h"
#include "EditorComponent.h"
#include "EditorTypes.h"
#include "PortScan.h"
#include "PhysicsControlComponent.h"
#include "StreamControlComponent.h"
#include "PhysicsTuning.h"
#include "PhysicsWorld.h"
#include "Renderer.h"
#include "Scene.h"
#include "Stats.h"
#include "VideoStreamer.h"
#include "WebRtcStreamer.h"
#include "math_bridge.h"

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
        // Something named in scene.cpp could not be loaded. Stopping is
        // deliberate: running on with a missing model or texture produces a
        // scene that silently differs from the configured one.
        LOGE("app", "%s", e.what());
        std::printf(
            "\nCheck the file name in src/scene.cpp and that the file sits "
            "next to wizengine.exe.\n(press Enter to close)\n");
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

static // ---- Streaming defaults -----------------------------------------------
// Engine-level output settings (deliberately NOT in SceneConfig.h - they
// describe how the engine ships pixels, not what is in the world). Codec can
// be overridden at launch with --codec, the encoder element (and thereby the
// GPU) with --encoder, and resolution/fps/bitrate per camera from the
// browser's System > Stream section.
//
// Codec guide: H264 = the compatibility baseline (every browser and device,
// hardware encode). H265 = better quality per bit, viewers Chrome 136+/
// Safari 18+ only (Edge and Firefox never offer it in WebRTC). AV1 = best
// compression, royalty free, Chrome/Edge/Firefox viewers, needs an
// AV1-capable GPU (RDNA3+/RTX40+/Arc) and rtpav1pay. VP9 = the software
// (CPU-encode) fallback. H265/AV1 fall back to H264 with a warning when
// their pieces are missing.
constexpr VideoCodec kDefaultCodec = VideoCodec::H264;
// 既定 1080p60 に見合う値（720p の頃は 4Mbps だった）。回線が細いときは
// ブラウザの Physics > Stream からカメラごとに下げられる。
constexpr int kVideoBitrate = 8000000;  // bits per second
// Colour conversion (RGBA -> NV12) on the GPU instead of a CPU core per
// camera. Automatically ignored (CPU path) when the encoder cannot take
// D3D11 memory - software encoders, VP9, non-Windows.
constexpr bool kGpuConvert = true;

int run(int argc, char** argv) {
    // Render resolution. The browser scales this up to fill the window, so it
    // stays cheap to render, encode and stream whatever the display size.
    // Raise it if the upscaled picture looks too soft (1920x1080 is ~2.2x the
    // pixels, and costs roughly that much more everywhere).
    constexpr int kWidth = 1920;
    constexpr int kHeight = 1080;
    constexpr int kFps = 60;

    // Usage:
    //   wizengine web [httpPort]         -> browser only: open the control UI
    //                                       (WebRTC video, no GStreamer output)
    //   wizengine window                 -> local preview window on this PC
    //   wizengine stream [host] [port]   -> RTP/UDP stream (default 127.0.0.1:5000)
    //   wizengine rtsp [url]             -> publish H.264 to an RTSP server
    // Default (no args) is web. The control UI is always available regardless.
    //
    // httpPort is the FIRST port: camera i is served on httpPort + i, so a
    // second instance needs a base far enough away not to overlap (with three
    // cameras, 8080 occupies 8080-8082).
    //
    // Options, accepted after the mode in any order. These are deployment
    // settings rather than scene content, which is why they live here and not
    // in scene.cpp:
    //   --physics-cores <spec>   cores the solver may use ("0-11", "0,2,4", "3")
    //   --render-cores  <spec>   cores for rendering and encoding
    //   --physics-threads <n>    solver worker threads (default: one per core)
    //   --max-cameras <n>        camera slots /cam0/../camN-1/ (1-16, default:
    //                            SceneConfig.h の kMaxCameras)
    //   --help                   print this list
    // Options first, so the positional parsing below only sees the rest.
    std::string physicsCoreSpec, renderCoreSpec;
    int physicsThreads = 0;
    int maxCameras = 0;  // 0 = SceneConfig.h の既定（kMaxCameras）
    // Codec from --codec; unset = kDefaultCodec above. The codec decides
    // the compression standard; --encoder then picks the concrete element
    // (and thereby the GPU) within it.
    std::optional<VideoCodec> codecOverride;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 < argc) return argv[++i];
            LOGW("app", "%s needs a value - ignoring", what);
            return {};
        };
        if (arg == "--physics-cores") {
            physicsCoreSpec = next("--physics-cores");
        } else if (arg == "--render-cores") {
            renderCoreSpec = next("--render-cores");
        } else if (arg == "--physics-threads") {
            const std::string v = next("--physics-threads");
            if (!v.empty()) physicsThreads = std::atoi(v.c_str());
        } else if (arg == "--max-cameras") {
            const std::string v = next("--max-cameras");
            if (!v.empty()) {
                maxCameras = std::atoi(v.c_str());
                if (maxCameras < 1 || maxCameras > 16) {
                    LOGW("app",
                         "--max-cameras %s is out of range (1-16) - using the "
                         "scene default",
                         v.c_str());
                    maxCameras = 0;
                }
            }
        } else if (arg == "--encoder") {
            const std::string v = next("--encoder");
            if (!v.empty()) WebRtcStreamer::setEncoderOverride(v);
        } else if (arg == "--codec") {
            std::string v = next("--codec");
            for (auto& ch : v)
                ch = char(std::tolower(static_cast<unsigned char>(ch)));
            if (v == "h264") codecOverride = VideoCodec::H264;
            else if (v == "h265" || v == "hevc") codecOverride = VideoCodec::H265;
            else if (v == "av1") codecOverride = VideoCodec::AV1;
            else if (v == "vp9") codecOverride = VideoCodec::VP9;
            else if (!v.empty())
                LOGW("app",
                     "--codec %s is not one of h264/h265/av1/vp9 - using the "
                     "scene default",
                     v.c_str());
        } else if (arg == "--help" || arg == "-h" || arg == "/?") {
            std::printf(
                "usage: wizengine [web [httpPort] | window | stream [host] "
                "[port] | rtsp [url]]\n"
                "                 [--physics-cores SPEC] [--render-cores SPEC]\n"
                "                 [--physics-threads N] [--max-cameras N]\n"
                "                 [--codec CODEC] [--encoder ELEMENT]\n\n"
                "SPEC is a core list: \"0-11\", \"0,2,4\", \"3\" or "
                "\"0-3,8,12-13\".\n"
                "Pinning keeps the solver and the video encoder off each "
                "other's cores.\n"
                "--max-cameras N sets the camera slots (pages /cam0/ .. "
                "/camN-1/, 1-16;\ndefault from SceneConfig.h). Video views "
                "are created when a camera is added.\n"
                "CODEC is h264 (default; every browser), h265 (Chrome/"
                "Safari only),\nav1 (needs an AV1-capable GPU) or vp9 "
                "(software). Unavailable codecs\nfall back to h264.\n"
                "ELEMENT is a GStreamer encoder element for that codec. "
                "GStreamer "
                "registers one\nper GPU (amfh264enc = primary, "
                "amfh264device1enc = next, ...), so this\npicks the encoding "
                "GPU. List them with: gst-inspect-1.0 amfcodec\n");
            return 0;
        } else {
            positional.push_back(arg);
        }
    }

    const std::string modeArg = positional.empty() ? "web" : positional[0];
    OutputMode outputMode = OutputMode::None;
    std::string host = "127.0.0.1";
    int port = 5000;
    int httpPort = 8080;  // base port for the browser UI, see below
    std::string rtspUrl = "rtsp://127.0.0.1:8554/wiz";
    if (modeArg == "window") {
        outputMode = OutputMode::Window;
    } else if (modeArg == "stream") {
        outputMode = OutputMode::Stream;
        if (argc > 2) host = argv[2];
        if (argc > 3) port = std::stoi(argv[3]);
    } else if (modeArg == "rtsp") {
        outputMode = OutputMode::Rtsp;
        if (positional.size() > 1) rtspUrl = positional[1];
    } else if (modeArg == "web" && positional.size() > 1) {
        httpPort = std::atoi(positional[1].c_str());
        if (httpPort < 1 || httpPort > 65535) {
            LOGW("http", "port '%s' is out of range - using 8080",
                 positional[1].c_str());
            httpPort = 8080;
        }
    }
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

    // Backend (core / multicore) is part of the scene configuration.
    const PhysicsBackend requested = scenePhysicsBackend();
    PhysicsWorld physics(requested);
    LOGI("app", "WizEngine %s", wizengine::engineVersion());
    LOGI("physics",
         "backend: requested=%s  compiled-in multicore=%s  using=%s",
         requested == PhysicsBackend::Multicore ? "multicore" : "core",
         multicoreAvailable() ? "yes" : "no", physics.backendName());
    wizengine::Renderer renderer(kWidth, kHeight, materialPath);
    // カメラスロット数は --max-cameras（未指定 = 0 なら SceneConfig の既定）。
    Scene scene(physics, renderer, std::size_t(maxCameras));
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
    // エディタモードの操作（配置・ジョイント・シーンの保存/読込）。物理の
    // レート系だけは PhysicsTuning を共有するので、それを渡しておく。
    scene.addComponent(std::make_unique<EditorComponent>(tuning));

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

            WebRtcStreamer::setPreferGpuConvert(kGpuConvert);
            ep->webrtc = std::make_unique<WebRtcStreamer>(
                kWidth, kHeight, kFps, codecOverride.value_or(kDefaultCodec),
                kVideoBitrate);
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
                return j.dump();
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

        while (g_run) {
            if (!gActive.load()) {
                // Nobody is watching: no stepping at all, and no backlog to
                // catch up on when a viewer returns.
                accumulator = 0.0;
                prev = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // 編集操作とモード切替はどちらのモードでも処理する（シミュレート
            // 中に設定を変えたり、物を足したりできるように）。Chrono を触る
            // のはこのスレッドだけ、という約束はここで守られる。
            scene.applyPendingEdits();

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
            const int substeps = tuning.substeps.load();
            const int hz = tuning.physicsHz.load();
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
            while (accumulator >= stepDt && stepped < kMaxCatchUp) {
                for (int i = 0; i < substeps; ++i) {
                    scene.stepPhysics(subDt);
                    const StepTimers t = physics.timers();  // seconds
                    solverMs += t.solver * 1000.0;
                    collisionMs += t.collision * 1000.0;
                }
                accumulator -= stepDt;
                simAdvanced += stepDt;
                simTotal += stepDt;
                ++stepped;
            }
            if (accumulator >= stepDt) {
                accumulator = 0.0;  // behind and out of catch-up budget
            }
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

        // Each camera turns its state into eye/target for its own view.
        for (std::size_t i = 0; i < endpoints.size(); ++i) {
            if (endpoints[i]->viewIndex == kNoView) continue;  // ビュー未生成
            scene.camera(i).applyTo(renderer, endpoints[i]->viewIndex);
        }
        if (endpoints.empty()) scene.camera(0).applyTo(renderer);

        // Copy the latest physics poses to the renderables and draw.
        scene.applyToRenderer();
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
    // Readbacks are asynchronous, so let the in-flight ones land before the
    // encoders and their buffers go away.
    renderer.finishPendingReadbacks();
    if (listener) listener->stop();
    std::puts("\nstopped.");
    return 0;
}
