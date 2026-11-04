#include "picture.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#ifdef __3DS__
#include <turbojpeg.h>
#endif

// stb_image is vendored unmodified (see source/stb/VENDORED.md); its own
// warnings are not ours to fix.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace picture {
namespace {

uint16_t rgb565(uint32_t red, uint32_t green, uint32_t blue)
{
    return static_cast<uint16_t>(((red >> 3) << 11) | ((green >> 2) << 5) |
                                 (blue >> 3));
}

// The box filter over the width x height part of the picture at (left, top),
// into exactly outWidth x outHeight. Enlarging repeats pixels.
void scaleArea(const uint8_t *pixels, uint32_t channels, uint32_t stride,
               uint32_t left, uint32_t top, uint32_t width, uint32_t height,
               uint32_t outWidth, uint32_t outHeight, uint16_t background,
               Picture &picture)
{
    picture.width = outWidth;
    picture.height = outHeight;
    picture.pixels.assign(static_cast<size_t>(outWidth) * outHeight, 0);

    const uint32_t backRed = ((background >> 11) & 0x1f) << 3;
    const uint32_t backGreen = ((background >> 5) & 0x3f) << 2;
    const uint32_t backBlue = (background & 0x1f) << 3;

    for (uint32_t y = 0; y < outHeight; ++y) {
        // The block of source pixels this output pixel covers.
        const uint32_t from = static_cast<uint32_t>(
            static_cast<uint64_t>(y) * height / outHeight);
        const uint32_t to = std::max(
            from + 1, static_cast<uint32_t>(static_cast<uint64_t>(y + 1) *
                                            height / outHeight));
        for (uint32_t x = 0; x < outWidth; ++x) {
            const uint32_t first = static_cast<uint32_t>(
                static_cast<uint64_t>(x) * width / outWidth);
            const uint32_t last = std::max(
                first + 1, static_cast<uint32_t>(static_cast<uint64_t>(x + 1) *
                                                 width / outWidth));
            uint64_t red = 0;
            uint64_t green = 0;
            uint64_t blue = 0;
            uint64_t count = 0;
            for (uint32_t sy = top + from; sy < top + to; ++sy) {
                const uint8_t *row =
                    pixels + (static_cast<size_t>(sy) * stride + left + first) *
                                 channels;
                for (uint32_t sx = first; sx < last; ++sx, row += channels) {
                    const uint32_t alpha = channels == 4 ? row[3] : 255;
                    red += (row[0] * alpha + backRed * (255 - alpha)) / 255;
                    green += (row[1] * alpha + backGreen * (255 - alpha)) / 255;
                    blue += (row[2] * alpha + backBlue * (255 - alpha)) / 255;
                    ++count;
                }
            }
            picture.pixels[static_cast<size_t>(y) * outWidth + x] =
                rgb565(static_cast<uint32_t>(red / count),
                       static_cast<uint32_t>(green / count),
                       static_cast<uint32_t>(blue / count));
        }
    }
}

// fit, or with fill the middle of the picture in the shape of maxWidth x
// maxHeight, scaled to exactly that.
void place(const uint8_t *pixels, uint32_t channels, uint32_t width,
           uint32_t height, uint32_t maxWidth, uint32_t maxHeight,
           uint16_t background, bool fill, Picture &picture)
{
    if (!fill) {
        fit(pixels, channels, width, height, maxWidth, maxHeight, background,
            picture);
        return;
    }
    uint32_t cropWidth = width;
    uint32_t cropHeight = height;
    if (static_cast<uint64_t>(width) * maxHeight >
        static_cast<uint64_t>(height) * maxWidth) {
        cropWidth = std::max<uint32_t>(
            1, static_cast<uint32_t>(static_cast<uint64_t>(height) * maxWidth /
                                     maxHeight));
    } else {
        cropHeight = std::max<uint32_t>(
            1, static_cast<uint32_t>(static_cast<uint64_t>(width) * maxHeight /
                                     maxWidth));
    }
    scaleArea(pixels, channels, width, (width - cropWidth) / 2,
              (height - cropHeight) / 2, cropWidth, cropHeight, maxWidth,
              maxHeight, background, picture);
    picture.sourceWidth = width;
    picture.sourceHeight = height;
}

std::string tooBig(int width, int height)
{
    return "At " + std::to_string(width) + " x " + std::to_string(height) +
           " pixels, this picture is too big to open on a 3DS.";
}

#ifdef __3DS__
bool isJpeg(const uint8_t *data, size_t size)
{
    return size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff;
}

// libjpeg-turbo can decode a JPEG at 1/2, 1/4 or 1/8 of its size, which
// takes a fraction of the memory and time. error is empty on success, and
// also when libjpeg-turbo could not start, for stb_image to try instead.
bool decodeJpeg(const uint8_t *data, size_t size, uint32_t maxWidth,
                uint32_t maxHeight, uint16_t background, bool fill,
                Picture &picture, std::string &error)
{
    tjhandle decoder = tj3Init(TJINIT_DECOMPRESS);
    if (decoder == nullptr) {
        return false;
    }
    bool ok = false;
    if (tj3DecompressHeader(decoder, data, size) == 0) {
        const int width = tj3Get(decoder, TJPARAM_JPEGWIDTH);
        const int height = tj3Get(decoder, TJPARAM_JPEGHEIGHT);
        // The smallest scale that still covers the screen.
        int count = 0;
        tjscalingfactor *factors = tj3GetScalingFactors(&count);
        tjscalingfactor best = {1, 1};
        uint64_t bestPixels = static_cast<uint64_t>(width) * height;
        for (int index = 0; factors != nullptr && index < count; ++index) {
            const tjscalingfactor factor = factors[index];
            const int scaledWidth = TJSCALED(width, factor);
            const int scaledHeight = TJSCALED(height, factor);
            const uint64_t scaledPixels =
                static_cast<uint64_t>(scaledWidth) * scaledHeight;
            const bool covers =
                scaledWidth >= std::min<int>(width, static_cast<int>(maxWidth)) &&
                scaledHeight >= std::min<int>(height, static_cast<int>(maxHeight));
            if (covers && scaledPixels < bestPixels) {
                best = factor;
                bestPixels = scaledPixels;
            }
        }
        const int scaledWidth = TJSCALED(width, best);
        const int scaledHeight = TJSCALED(height, best);
        if (bestPixels > MaxSourcePixels) {
            error = tooBig(width, height);
        } else if (tj3SetScalingFactor(decoder, best) == 0) {
            std::vector<unsigned char> rgb(bestPixels * 3);
            if (tj3Decompress8(decoder, data, size, rgb.data(), 0, TJPF_RGB) == 0) {
                place(rgb.data(), 3, static_cast<uint32_t>(scaledWidth),
                      static_cast<uint32_t>(scaledHeight), maxWidth, maxHeight,
                      background, fill, picture);
                picture.sourceWidth = static_cast<uint32_t>(width);
                picture.sourceHeight = static_cast<uint32_t>(height);
                ok = true;
            }
        }
    }
    if (!ok && error.empty()) {
        const char *reason = tj3GetErrorStr(decoder);
        error = std::string("The picture could not be decoded") +
                (reason != nullptr ? std::string(" (") + reason + ")" : "") +
                ".";
    }
    tj3Destroy(decoder);
    return ok;
}

// handled is set once the file is known to be a JPEG, so a failure is not
// retried with stb_image.
bool loadJpeg(const std::string &path, uint32_t maxWidth, uint32_t maxHeight,
              uint16_t background, bool fill, Picture &picture,
              std::string &error, bool &handled)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    unsigned char magic[3] = {};
    const bool jpeg = std::fread(magic, 1, 3, file) == 3 && isJpeg(magic, 3);
    long length = -1;
    if (jpeg && std::fseek(file, 0, SEEK_END) == 0) {
        length = std::ftell(file);
    }
    // A JPEG over 32 MiB would not fit next to its decoded pixels anyway.
    if (!jpeg || length <= 0 || length > 32 * 1024 * 1024) {
        std::fclose(file);
        return false;
    }
    handled = true;
    std::vector<unsigned char> bytes(static_cast<size_t>(length));
    std::rewind(file);
    const bool read = std::fread(bytes.data(), 1, bytes.size(), file) ==
                      bytes.size();
    std::fclose(file);
    if (!read) {
        error = "The picture could not be read.";
        return false;
    }
    const bool ok = decodeJpeg(bytes.data(), bytes.size(), maxWidth,
                               maxHeight, background, fill, picture, error);
    handled = ok || !error.empty();
    return ok;
}
#endif

// Places what stb_image decoded, or explains why it could not.
bool takeDecoded(stbi_uc *decoded, int width, int height, int channels,
                 uint32_t maxWidth, uint32_t maxHeight, uint16_t background,
                 bool fill, Picture &picture, std::string &error)
{
    if (decoded == nullptr) {
        const char *reason = stbi_failure_reason();
        error = std::string("The picture could not be decoded") +
                (reason != nullptr ? std::string(" (") + reason + ")" : "") +
                ". It may be damaged, or too big for the memory left.";
        return false;
    }
    place(decoded, static_cast<uint32_t>(channels),
          static_cast<uint32_t>(width), static_cast<uint32_t>(height),
          maxWidth, maxHeight, background, fill, picture);
    stbi_image_free(decoded);
    return true;
}

} // namespace

bool size(const std::string &path, uint32_t &width, uint32_t &height)
{
    int w = 0;
    int h = 0;
    int components = 0;
    if (!stbi_info(path.c_str(), &w, &h, &components) || w <= 0 || h <= 0) {
        return false;
    }
    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
    return true;
}

void fittedSize(uint32_t width, uint32_t height, uint32_t maxWidth,
                uint32_t maxHeight, uint32_t &outWidth, uint32_t &outHeight)
{
    // Enlarge small pictures by a whole factor, shrink big ones to fit.
    if (width == 0 || height == 0) {
        outWidth = outHeight = 0;
    } else if (width > maxWidth || height > maxHeight) {
        const double scale = std::min(static_cast<double>(maxWidth) / width,
                                      static_cast<double>(maxHeight) / height);
        outWidth = std::max<uint32_t>(1, static_cast<uint32_t>(width * scale));
        outHeight =
            std::max<uint32_t>(1, static_cast<uint32_t>(height * scale));
    } else {
        const uint32_t factor = std::max<uint32_t>(
            1, std::min(maxWidth / width, maxHeight / height));
        outWidth = width * factor;
        outHeight = height * factor;
    }
}

void fit(const uint8_t *pixels, uint32_t channels, uint32_t width,
         uint32_t height, uint32_t maxWidth, uint32_t maxHeight,
         uint16_t background, Picture &picture)
{
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
    fittedSize(width, height, maxWidth, maxHeight, outWidth, outHeight);
    scaleArea(pixels, channels, width, 0, 0, width, height, outWidth,
              outHeight, background, picture);
    picture.sourceWidth = width;
    picture.sourceHeight = height;
}

bool load(const std::string &path, uint32_t maxWidth, uint32_t maxHeight,
          uint16_t background, Picture &picture, std::string &error,
          bool fill)
{
    picture = Picture();
#ifdef __3DS__
    bool handled = false;
    if (loadJpeg(path, maxWidth, maxHeight, background, fill, picture, error,
                 handled) ||
        handled) {
        return error.empty();
    }
#endif
    int width = 0;
    int height = 0;
    int components = 0;
    if (!stbi_info(path.c_str(), &width, &height, &components) ||
        width <= 0 || height <= 0) {
        error = "This picture's format is not supported.";
        return false;
    }
    if (static_cast<uint64_t>(width) * height > MaxSourcePixels) {
        error = tooBig(width, height);
        return false;
    }
    // Only pictures with transparency pay for the fourth byte.
    const int channels = components == 2 || components == 4 ? 4 : 3;
    int ignored = 0;
    stbi_uc *decoded =
        stbi_load(path.c_str(), &width, &height, &ignored, channels);
    return takeDecoded(decoded, width, height, channels, maxWidth, maxHeight,
                       background, fill, picture, error);
}

bool decode(const std::vector<uint8_t> &bytes, uint32_t maxWidth,
            uint32_t maxHeight, uint16_t background, Picture &picture,
            std::string &error, bool fill)
{
    picture = Picture();
#ifdef __3DS__
    if (isJpeg(bytes.data(), bytes.size()) &&
        (decodeJpeg(bytes.data(), bytes.size(), maxWidth, maxHeight,
                    background, fill, picture, error) ||
         !error.empty())) {
        return error.empty();
    }
#endif
    const int length = static_cast<int>(std::min<size_t>(bytes.size(), INT32_MAX));
    int width = 0;
    int height = 0;
    int components = 0;
    if (!stbi_info_from_memory(bytes.data(), length, &width, &height,
                               &components) ||
        width <= 0 || height <= 0) {
        error = "This picture's format is not supported.";
        return false;
    }
    if (static_cast<uint64_t>(width) * height > MaxSourcePixels) {
        error = tooBig(width, height);
        return false;
    }
    const int channels = components == 2 || components == 4 ? 4 : 3;
    int ignored = 0;
    stbi_uc *decoded = stbi_load_from_memory(bytes.data(), length, &width,
                                             &height, &ignored, channels);
    return takeDecoded(decoded, width, height, channels, maxWidth, maxHeight,
                       background, fill, picture, error);
}

} // namespace picture
