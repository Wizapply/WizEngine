#pragma once

#include <cstddef>
#include <functional>

#include <nlohmann/json.hpp>

#include "SceneComponent.h"

// Routes the browser's per-camera stream-format command
//   {"cmd":"stream","w":1280,"h":720,"fps":30,"kbps":4000}
// to main, which owns the endpoints: main passes an apply callback at
// construction, stores the values, and its render loop resizes the view and
// updates the streamer at a safe point. The browser reconnects afterwards -
// the new format takes effect on that negotiation.
//
// The ranges are sanity clamps against hand-crafted requests, wide enough for
// anything the UI offers.
class StreamControlComponent : public SceneComponent {
public:
    using Apply =
        std::function<void(std::size_t cam, int w, int h, int fps, int kbps)>;

    explicit StreamControlComponent(Apply apply) : apply_(std::move(apply)) {}

    bool onCommand(Scene& scene, std::size_t camIndex,
                   const nlohmann::json& msg) override {
        (void)scene;
        if (msg.value("cmd", "") != "stream") return false;
        const int w = msg.value("w", 0);
        const int h = msg.value("h", 0);
        const int fps = msg.value("fps", 0);
        const int kbps = msg.value("kbps", 0);
        if (w < 320 || w > 3840 || h < 180 || h > 2160) return true;
        if (fps < 5 || fps > 120) return true;
        if (kbps < 250 || kbps > 50000) return true;
        // Even dimensions: 4:2:0 encoders reject odd sizes.
        if (apply_) apply_(camIndex, w & ~1, h & ~1, fps, kbps);
        return true;
    }

private:
    Apply apply_;
};
