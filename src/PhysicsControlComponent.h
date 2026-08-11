#pragma once

#include <nlohmann/json.hpp>

#include "Log.h"
#include "PhysicsTuning.h"
#include "Scene.h"
#include "SceneComponent.h"

// Routes the engine-tuning commands from the browser (reset, pause, solver,
// envelope, recovery, rate, substeps) into the shared PhysicsTuning state,
// which the physics loop in main.cpp applies at a step boundary.
//
// This used to be an if/else chain in main.cpp's input thread, after the
// scene components had their turn. As a component it goes through the same
// dispatchCommand() path as everything else, so main.cpp no longer parses
// any command itself. main still owns the PhysicsTuning (it drives the
// physics loop) and passes it in at construction.
//
// ここで変えた値は EditorState のシミュレート設定にも映す（mirror）。エディタ
// のシーン保存はそちらを書き出すので、映しておかないと「画面で見ている物理」
// と「保存される物理」が食い違う。setSim は mutex 持ちなので INPUT スレッド
// から呼んでよい。
//
// The ranges below are sanity clamps against a hand-crafted request, not
// tuning advice - the UI never sends values outside them.
class PhysicsControlComponent : public SceneComponent {
public:
    explicit PhysicsControlComponent(PhysicsTuning& tuning)
        : tuning_(tuning) {}

    bool onCommand(Scene& scene, std::size_t camIndex,
                   const nlohmann::json& msg) override {
        (void)camIndex;
        const std::string cmd = msg.value("cmd", "");
        auto mirror = [&scene](auto&& set) {
            wizengine::editor::SimSettings s = scene.editor().sim();
            set(s);
            scene.editor().setSim(s);
        };
        if (cmd == "reset") {
            tuning_.reset.store(true);
        } else if (cmd == "pause") {
            tuning_.paused.store(!tuning_.paused.load());
        } else if (cmd == "solver") {
            const int iters = msg.value("iterations", 0);
            if (iters > 0 && iters <= 2000) {
                tuning_.pendingIterations.store(iters);
                mirror([iters](wizengine::editor::SimSettings& s) {
                    s.iterations = iters;
                });
                LOGI("physics", "solver iterations -> %d", iters);
            }
        } else if (cmd == "envelope") {
            const double env = msg.value("m", 0.0);
            if (env > 0.0 && env <= 0.2) {
                tuning_.pendingEnvelope.store(env);
                mirror([env](wizengine::editor::SimSettings& s) {
                    s.envelope = env;
                });
                LOGI("physics", "collision envelope -> %.4f m", env);
            }
        } else if (cmd == "recovery") {
            const double rec = msg.value("v", 0.0);
            if (rec > 0.0 && rec <= 10.0) {
                tuning_.pendingRecovery.store(rec);
                mirror([rec](wizengine::editor::SimSettings& s) {
                    s.recovery = rec;
                });
                LOGI("physics", "contact recovery speed -> %.2f m/s", rec);
            }
        } else if (cmd == "rate") {
            const int hz = msg.value("hz", 0);
            if (hz >= 10 && hz <= 240) {
                tuning_.physicsHz.store(hz);
                mirror([hz](wizengine::editor::SimSettings& s) { s.hz = hz; });
                LOGI("physics", "rate -> %d Hz", hz);
            }
        } else if (cmd == "substeps") {
            const int n = msg.value("n", 0);
            if (n >= 1 && n <= 8) {
                tuning_.substeps.store(n);
                mirror([n](wizengine::editor::SimSettings& s) {
                    s.substeps = n;
                });
                LOGI("physics", "substeps -> %d", n);
            }
        } else {
            return false;  // not ours - let the next component look at it
        }
        return true;
    }

private:
    PhysicsTuning& tuning_;
};
