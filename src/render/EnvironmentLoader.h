#pragma once

#include <string>

namespace filament {
class Engine;
class IndirectLight;
class Texture;
}  // namespace filament

namespace wizengine {

// The two GPU objects an environment amounts to. The caller owns both and
// destroys them (in this order is fine) when replacing the environment or
// shutting down; `light` references `reflections`, so keep them together.
struct EnvironmentIBL {
    filament::IndirectLight* light = nullptr;
    filament::Texture* reflections = nullptr;
};

// Loads a Radiance .hdr equirectangular panorama (resolved via assetPath) and
// prefilters it on the GPU into an image-based light: decode with stb, upload
// mip 0, then IBLPrefilterContext turns it into a cubemap and a roughness mip
// chain - the same work cmgen does offline, done at load time so any .hdr can
// be used without a build step. Panoramas wider than 2048 px are downsampled
// on the CPU first.
//
// Throws AssetError with a diagnosis (missing file, wrong format, not 2:1,
// GPU object creation failure) - never returns a half-built environment.
EnvironmentIBL loadEnvironmentIBL(filament::Engine& engine,
                                  const std::string& hdrName, float intensity);

}  // namespace wizengine
