#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Load an image file (PNG, JPEG, TGA, BMP, ...) as tightly packed RGBA8.
// Returns false if the file is missing or cannot be decoded, leaving the
// outputs untouched. Backed by stb_image.
// Loads a floating-point HDR/RadianceHDR (.hdr) image as RGB, three floats per
// pixel. Used for the equirectangular environment map: unlike an LDR texture
// its values go above 1.0, which is what makes bright sky and lamps read as
// light sources rather than white pixels.
// Why the last stb load failed, for error messages. Never null.
const char* imageFailureReason();

// Identifies a file from its first bytes, e.g. "Radiance HDR", "OpenEXR",
// "PNG". Used in error messages: "unknown image type" is not much help when
// the real problem is that the file is an .exr with an .hdr name.
std::string identifyImageFormat(const std::string& path);

// First bytes of a file, escaped for printing. For diagnosing a file that
// looks right but no decoder will accept.
std::string describeHeaderBytes(const std::string& path);

// maxWidth: downsample by an integer factor until the image is no wider than
// this. An 8k panorama is 400 MB as floats and gains nothing here - the
// prefilter reduces it to a small cubemap anyway - and that allocation can
// simply fail. 0 disables the limit.
bool loadImageHDR(const std::string& path, std::vector<float>& out, int& width,
                  int& height, int maxWidth = 0);

bool loadImageRGBA(const std::string& path, std::vector<uint8_t>& outPixels,
                   int& outWidth, int& outHeight);
