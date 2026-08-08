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
#include "AssetError.h"
#include "CpuAffinity.h"
#include "PortScan.h"
#include "PhysicsControlComponent.h"
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

static int run(int argc, char** argv) {
    // Render resolution. The browser scales this up to fill the window, so it
    // stays cheap to render, encode and stream whatever the display size.
    // Raise it if the upscaled picture looks too soft (1920x1080 is ~2.2x the
    // pixels, and costs roughly that much more everywhere).
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;
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
    //   --help                   print this list
    // Options first, so the positional parsing below only sees the rest.
    std::string physicsCoreSpec, renderCoreSpec;
    int physicsThreads = 0;
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
        } else if (arg == "--help" || arg == "-h" || arg == "/?") {
            std::printf(
                "usage: wizengine [web [httpPort] | window | stream [host] "
                "[port] | rtsp [url]]\n"
                "                 [--physics-cores SPEC] [--render-cores SPEC]\n"
                "                 [--physics-threads N]\n\n"
                "SPEC is a core list: \"0-11\", \"0,2,4\", \"3\" or "
                "\"0-3,8,12-13\".\n"
                "Pinning keeps the solver and the video encoder off each "
                "other's cores.\n");
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
    LOGI("physics",
         "backend: requested=%s  compiled-in multicore=%s  using=%s",
         requested == PhysicsBackend::Multicore ? "multicore" : "core",
         multicoreAvailable() ? "yes" : "no", physics.backendName());
    wizengine::Renderer renderer(kWidth, kHeight, materialPath);
    Scene scene(physics, renderer);
    LOGI("physics", "rate: %d Hz (scene default), substeps %d",
         scene.physicsHz(), scene.substeps());
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

    struct CameraEndpoint {
        std::unique_ptr<HttpServer> http;
        std::unique_ptr<WebRtcStreamer> webrtc;
        std::function<void(const uint8_t*, size_t)> pushFrame;
        std::size_t viewIndex = 0;
        int port = 0;
    };
    std::vector<std::unique_ptr<CameraEndpoint>> endpoints;

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

    PerfStats stats;
    stats.substeps.store(scene.substeps());
    stats.iterations.store(scene.solverIterations());

    if (outputMode == OutputMode::None) {
        int nextPort = httpPort;
        for (std::size_t i = 0; i < scene.cameraCount(); ++i) {
            auto ep = std::make_unique<CameraEndpoint>();
            // Take the next free port at or after the one this camera would
            // normally get, so a port already in use (another instance, or an
            // unrelated program) is skipped instead of failing to bind.
            ep->port = wizengine::findFreePortRun(nextPort, 1);
            nextPort = ep->port + 1;
            // Camera 0 uses the view the renderer already created.
            ep->viewIndex = (i == 0) ? 0 : renderer.addView();

            ep->webrtc = std::make_unique<WebRtcStreamer>(
                kWidth, kHeight, kFps, sceneVideoCodec(), sceneVideoBitrate());
            ep->http = std::make_unique<HttpServer>(ep->port, "assets/web");

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
                j["engine"] = physics.backendName();
                j["codec"] = endpoints[camIndex]->webrtc->codecName();
                j["camera"] = int(camIndex);
                j["cameraCount"] = int(scene.cameraCount());
                return j.dump();
            });

            // Hierarchy for the sidebar: cameras and objects, with this
            // page's own camera marked so it can show its selection.
            ep->http->setSceneProvider([&, camIndex] {
                // Ports may not be consecutive (busy ones are skipped), so the
                // page is told the real list rather than computing it.
                nlohmann::json j = nlohmann::json::parse(
                    scene.hierarchyJson(camIndex), nullptr, false);
                if (j.is_discarded()) return scene.hierarchyJson(camIndex);
                for (auto& e : endpoints) {
                    j["ports"].push_back(e->port);
                    // Whether someone is watching that camera right now, so the
                    // sidebar can show which ones are taken - only one browser
                    // at a time can view a given camera.
                    j["busy"].push_back(e->http->hasViewer());
                }
                return j.dump();
            });

            endpoints.push_back(std::move(ep));
        }
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

    for (auto& ep : endpoints) ep->http->start();
    for (auto& ep : endpoints) {
        LOGI("http", "camera %zu: http://127.0.0.1:%d/", ep->viewIndex,
             ep->port);
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

        // Each camera turns its state into eye/target for its own view.
        for (std::size_t i = 0; i < endpoints.size(); ++i) {
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
            for (auto& ep : endpoints) {
                if (!ep->http->hasViewer()) continue;
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
    for (auto& ep : endpoints) ep->http->stop();
    std::puts("\nstopped.");
    return 0;
}
