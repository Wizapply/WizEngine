#include "EnvironmentLoader.h"

#include <filament/Engine.h>
#include <filament/IndirectLight.h>
#include <filament/Texture.h>
#include <filament-iblprefilter/IBLPrefilterContext.h>

#include <backend/PixelBufferDescriptor.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "AssetError.h"
#include "ImageLoader.h"
#include "Log.h"

using namespace filament;

namespace wizengine {

EnvironmentIBL loadEnvironmentIBL(filament::Engine& engine,
                                  const std::string& hdrName,
                                  float intensity) {
    const std::string path = assetPath(hdrName);
    std::vector<float> pixels;
    int w = 0, h = 0;
    // 2048 wide is ample: the prefilter turns this into a 256 px cubemap.
    if (!loadImageHDR(path, pixels, w, h, 2048)) {
        // Separate "not there" from "there but unreadable": the two need
        // completely different fixes, and the previous message covered both.
        std::ifstream probe(path, std::ios::binary);
        if (!probe) {
            wizengine::requireFile(hdrName, "environment map");  // names the cwd
        }
        throw AssetError(path,
                         "could not be decoded (" +
                             std::string(imageFailureReason()) +
                             "). The file looks like: " +
                             identifyImageFormat(path) +
                             ". A Radiance .hdr (RGBE) panorama is expected.\n"
                             "  header bytes: " +
                             describeHeaderBytes(path));
    }

    if (w != 2 * h) {
        throw AssetError(path,
                         "is not equirectangular (width must be twice the "
                         "height)");
    }

    // Upload the panorama: one float3 per pixel. The prefilter requires a
    // texture WITH a full mip chain - it samples lower mips for the rougher
    // reflection levels. The prefilter's precondition is exact: the equirect
    // must have ilogb(width)+1 levels allocated, no more, no less. Width is
    // the larger side here (w == 2*h), so counting from max(w, h) matches.
    uint32_t levels = 1;
    for (int dim = std::max(w, h); dim > 1; dim >>= 1) ++levels;
    // GEN_MIPMAPPABLE is required: EquirectangularToCubemap calls
    // generateMipmaps() on this texture unconditionally ("we need mipmaps
    // because we're sampling down"), and without the flag that call hits a
    // precondition assert and aborts. This is also why no mip data is
    // uploaded below: Filament regenerates every level from mip 0 on the
    // GPU anyway, overwriting anything uploaded by hand.
    Texture* equirect = Texture::Builder()
                            .width(uint32_t(w))
                            .height(uint32_t(h))
                            .levels(uint8_t(levels))
                            .format(Texture::InternalFormat::R11F_G11F_B10F)
                            .sampler(Texture::Sampler::SAMPLER_2D)
                            .usage(Texture::Usage::DEFAULT |
                                   Texture::Usage::GEN_MIPMAPPABLE)
                            .build(engine);
    if (!equirect) throw AssetError(path, "could not create the environment texture");

    // Upload mip 0 only; the rest of the chain is generated on the GPU by
    // the prefilter (see the usage flag above).
    {
        const std::size_t count = std::size_t(w) * std::size_t(h) * 3;
        auto* buf = new float[count];
        std::memcpy(buf, pixels.data(), count * sizeof(float));
        Texture::PixelBufferDescriptor pbd(
            buf, count * sizeof(float), Texture::Format::RGB,
            Texture::Type::FLOAT,
            [](void* b, size_t, void*) { delete[] static_cast<float*>(b); });
        equirect->setImage(engine, 0, std::move(pbd));
    }


    // Prefilter on the GPU: equirect -> cubemap -> roughness mip chain. This is
    // what cmgen does offline, done here at load time instead so the scene can
    // name any .hdr without a build step.
    // IBLPrefilterContext lives in the global namespace, not filament::.
    IBLPrefilterContext context(engine);
    IBLPrefilterContext::EquirectangularToCubemap toCubemap(context);
    Texture* cubemap = toCubemap(equirect);
    engine.destroy(equirect);  // no longer needed once converted
    if (!cubemap) throw AssetError(path, "could not be converted to a cubemap");

    IBLPrefilterContext::SpecularFilter filter(context);
    Texture* reflections = filter(cubemap);
    engine.destroy(cubemap);
    if (!reflections) throw AssetError(path, "could not be prefiltered");

    // No irradiance coefficients are given: Filament then derives the diffuse
    // term from the reflection map's lowest mip, which is what we want here.
    IndirectLight* env = IndirectLight::Builder()
                             .reflections(reflections)
                             .intensity(intensity)
                             .build(engine);
    if (!env) {
        engine.destroy(reflections);
        throw AssetError(path, "could not build the indirect light");
    }

    LOGI("ibl", "%s (%dx%d, intensity %.0f)", hdrName.c_str(), w, h,
         intensity);
    return {env, reflections};
}

}  // namespace wizengine
