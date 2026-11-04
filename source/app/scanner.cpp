#include "scanner.hpp"

#include "crash.hpp"
#include "image.hpp"
#include "text.hpp"
#include "ui.hpp"
#include "url.hpp"

#include <3ds.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>

extern "C" {
#include "quirc.h"
}

namespace scanner {
namespace {

constexpr u16 FrameWidth = 400;
constexpr u16 FrameHeight = 240;
constexpr size_t FramePixels = FrameWidth * FrameHeight;
constexpr size_t FrameBytes = FramePixels * sizeof(u16);
constexpr u16 TextureWidth = 512;
constexpr u16 TextureHeight = 256;
constexpr s64 FrameTimeoutNs = 1000000000LL;
constexpr int MaxConsecutiveStalls = 5;
constexpr size_t CameraStackSize = 32 * 1024;
constexpr u64 NoticeDurationMs = 2500;

constexpr const char *LookingText = "Looking for a QR code...";

const Tex3DS_SubTexture PreviewSubTexture = {
    TextureWidth, TextureHeight, 0.0f, 1.0f, 1.0f, 0.0f};

// The preview alternates between two textures, so the CPU never rewrites the
// one the GPU drew last, and switching textures every frame makes citro2d
// rebind it, which also clears the GPU texture cache. They are static so the
// pointers citro2d keeps to the last bound texture never dangle.
C3D_Tex previewTextures[2];

std::string failureText(const char *operation, Result result)
{
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08lX).", operation,
                  static_cast<unsigned long>(result));
    return buffer;
}

bool isTimeout(Result result)
{
    return R_DESCRIPTION(result) == RD_TIMEOUT;
}

// Owns the outer camera. A dedicated thread keeps a receive request queued at
// all times, so the camera's small FIFO does not overflow while the main
// thread decodes. Frames are handed over through three buffers: the camera
// fills "back", the newest complete frame waits in "ready", and the main
// thread reads "front".
class Camera {
public:
    bool open(std::string &error);
    void close();

    // The newest frame, or nullptr if none arrived since the last call. The
    // pointer stays valid until the next call.
    const u16 *takeFrame();
    bool failed() const { return failed_.load(); }

private:
    static void threadMain(void *argument)
    {
        crash::ThreadName name("camera");
        static_cast<Camera *>(argument)->run();
    }
    void run();
    void restartCapture(Handle &receive);

    u16 *buffers_[3] = {nullptr, nullptr, nullptr};
    int back_ = 0;
    int ready_ = 1;
    int front_ = 2;
    bool fresh_ = false;
    LightLock lock_ {};
    Handle stopEvent_ = 0;
    Handle bufferErrorEvent_ = 0;
    Thread thread_ = nullptr;
    u32 transferUnit_ = 0;
    bool initialized_ = false;
    std::atomic<bool> failed_ {false};
};

bool Camera::open(std::string &error)
{
    LightLock_Init(&lock_);

    Result result = camInit();
    if (R_FAILED(result)) {
        error = failureText("Starting the camera service", result);
        return false;
    }
    initialized_ = true;

    result = CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A);
    if (R_SUCCEEDED(result)) {
        result = CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
    }
    if (R_SUCCEEDED(result)) {
        result = CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_30);
    }
    if (R_SUCCEEDED(result)) {
        result = CAMU_SetNoiseFilter(SELECT_OUT1, true);
    }
    if (R_SUCCEEDED(result)) {
        result = CAMU_SetAutoExposure(SELECT_OUT1, true);
    }
    if (R_SUCCEEDED(result)) {
        result = CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
    }
    if (R_SUCCEEDED(result)) {
        result = CAMU_Activate(SELECT_OUT1);
    }
    if (R_SUCCEEDED(result)) {
        result = CAMU_SetTrimming(PORT_CAM1, false);
    }
    if (R_SUCCEEDED(result)) {
        result = CAMU_GetMaxBytes(&transferUnit_, FrameWidth, FrameHeight);
    }
    if (R_SUCCEEDED(result)) {
        result = CAMU_SetTransferBytes(PORT_CAM1, transferUnit_, FrameWidth,
                                       FrameHeight);
    }
    if (R_SUCCEEDED(result)) {
        result = CAMU_GetBufferErrorInterruptEvent(&bufferErrorEvent_,
                                                   PORT_CAM1);
    }
    if (R_FAILED(result)) {
        error = failureText("Setting up the camera", result);
        return false;
    }

    for (u16 *&buffer : buffers_) {
        buffer = static_cast<u16 *>(linearAlloc(FrameBytes));
        if (buffer == nullptr) {
            error = "Not enough memory for the camera.";
            return false;
        }
        std::memset(buffer, 0, FrameBytes);
        // Nothing dirty may be written back over a frame the camera delivers.
        GSPGPU_FlushDataCache(buffer, FrameBytes);
    }

    result = svcCreateEvent(&stopEvent_, RESET_ONESHOT);
    if (R_SUCCEEDED(result)) {
        result = CAMU_ClearBuffer(PORT_CAM1);
    }
    if (R_SUCCEEDED(result)) {
        result = CAMU_StartCapture(PORT_CAM1);
    }
    if (R_FAILED(result)) {
        error = failureText("Starting the camera", result);
        return false;
    }

    // The camera thread mostly waits, so it runs just above the main thread
    // and picks up every frame promptly.
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    thread_ = threadCreate(threadMain, this, CameraStackSize,
                           std::max(0x18, static_cast<int>(priority) - 1), -2,
                           false);
    if (thread_ == nullptr) {
        error = "Could not start the camera thread.";
        return false;
    }
    return true;
}

void Camera::restartCapture(Handle &receive)
{
    if (receive != 0) {
        svcCloseHandle(receive);
        receive = 0;
    }
    CAMU_StopCapture(PORT_CAM1);
    CAMU_ClearBuffer(PORT_CAM1);
    CAMU_StartCapture(PORT_CAM1);
}

void Camera::run()
{
    Handle receive = 0;
    int stalls = 0;

    while (true) {
        if (receive == 0 &&
            R_FAILED(CAMU_SetReceiving(&receive, buffers_[back_], PORT_CAM1,
                                       FrameBytes,
                                       static_cast<s16>(transferUnit_)))) {
            failed_ = true;
            break;
        }

        Handle handles[3] = {stopEvent_, bufferErrorEvent_, receive};
        s32 index = -1;
        const Result result =
            svcWaitSynchronizationN(&index, handles, 3, false, FrameTimeoutNs);

        // A timeout is not an error code (R_FAILED is false for it), so it
        // has to be checked explicitly. The camera stops after sleep mode or
        // a lost transfer; restarting the capture brings it back.
        if (isTimeout(result)) {
            if (++stalls >= MaxConsecutiveStalls) {
                failed_ = true;
                break;
            }
            restartCapture(receive);
            continue;
        }
        if (R_FAILED(result)) {
            failed_ = true;
            break;
        }
        if (index == 0) {
            break;
        }
        if (index == 1) {
            // Buffer error: the FIFO overflowed and capture has stopped.
            svcClearEvent(bufferErrorEvent_);
            restartCapture(receive);
            continue;
        }

        svcCloseHandle(receive);
        receive = 0;
        stalls = 0;
        GSPGPU_InvalidateDataCache(buffers_[back_], FrameBytes);

        LightLock_Lock(&lock_);
        std::swap(back_, ready_);
        fresh_ = true;
        LightLock_Unlock(&lock_);
    }

    if (receive != 0) {
        svcCloseHandle(receive);
    }
}

const u16 *Camera::takeFrame()
{
    LightLock_Lock(&lock_);
    const bool fresh = fresh_;
    if (fresh) {
        std::swap(front_, ready_);
        fresh_ = false;
    }
    LightLock_Unlock(&lock_);
    return fresh ? buffers_[front_] : nullptr;
}

void Camera::close()
{
    if (thread_ != nullptr) {
        svcSignalEvent(stopEvent_);
        threadJoin(thread_, U64_MAX);
        threadFree(thread_);
        thread_ = nullptr;
    }
    if (initialized_) {
        CAMU_StopCapture(PORT_CAM1);
        CAMU_Activate(SELECT_NONE);
    }
    if (bufferErrorEvent_ != 0) {
        svcCloseHandle(bufferErrorEvent_);
        bufferErrorEvent_ = 0;
    }
    if (stopEvent_ != 0) {
        svcCloseHandle(stopEvent_);
        stopEvent_ = 0;
    }
    if (initialized_) {
        camExit();
        initialized_ = false;
    }
    for (u16 *&buffer : buffers_) {
        if (buffer != nullptr) {
            linearFree(buffer);
            buffer = nullptr;
        }
    }
}

bool createTextures()
{
    for (int index = 0; index < 2; ++index) {
        C3D_Tex &texture = previewTextures[index];
        if (!C3D_TexInit(&texture, TextureWidth, TextureHeight,
                         GPU_RGB565)) {
            for (int created = 0; created < index; ++created) {
                C3D_TexDelete(&previewTextures[created]);
            }
            return false;
        }
        std::memset(texture.data, 0, texture.size);
        C3D_TexSetFilter(&texture, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        C3D_TexFlush(&texture);
    }
    return true;
}

// Returns the first successfully decoded payload in the current image.
bool decodeAny(quirc *decoder, std::string &payload)
{
    const int count = quirc_count(decoder);
    for (int index = 0; index < count; ++index) {
        quirc_code code;
        quirc_data data;
        quirc_extract(decoder, index, &code);
        quirc_decode_error_t status = quirc_decode(&code, &data);
        if (status == QUIRC_ERROR_DATA_ECC) {
            // Codes seen in a mirror or through the back of a screen.
            quirc_flip(&code);
            status = quirc_decode(&code, &data);
        }
        if (status == QUIRC_SUCCESS) {
            payload.assign(reinterpret_cast<const char *>(data.payload),
                           static_cast<size_t>(data.payload_len));
            return true;
        }
    }
    return false;
}

} // namespace

std::string scan(bool *typeInstead)
{
    if (typeInstead != nullptr) {
        *typeInstead = false;
    }
    Camera camera;
    std::string error;
    if (!camera.open(error)) {
        camera.close();
        ui::message("Camera unavailable", error, ui::Tone::Error);
        return "";
    }

    quirc *decoder = quirc_new();
    if (decoder == nullptr ||
        quirc_resize(decoder, FrameWidth, FrameHeight) < 0) {
        if (decoder != nullptr) {
            quirc_destroy(decoder);
        }
        camera.close();
        ui::message("Scanner unavailable", "Not enough memory to decode QR "
                    "codes.", ui::Tone::Error);
        return "";
    }
    if (!createTextures()) {
        quirc_destroy(decoder);
        camera.close();
        ui::message("Scanner unavailable", "Not enough memory for the "
                    "camera preview.", ui::Tone::Error);
        return "";
    }

    const C2D_Image images[2] = {{&previewTextures[0], &PreviewSubTexture},
                                 {&previewTextures[1], &PreviewSubTexture}};
    int shown = -1;
    std::string status = LookingText;
    u32 statusColor = ui::color::Success;
    u64 noticeUntil = 0;
    std::string found;

    ui::enterScreen("scanner");
    while (ui::keepRunning()) {
        const ui::Input input = ui::readInput();
        if ((input.down & (KEY_B | KEY_START)) ||
            input.tapped(ui::ScannerCancelButton)) {
            break;
        }
        if ((input.down & KEY_Y) || input.tapped(ui::ScannerTypeButton)) {
            if (typeInstead != nullptr) {
                *typeInstead = true;
            }
            break;
        }
        if (camera.failed()) {
            error = "The camera stopped sending pictures. Close the scanner "
                    "and try again.";
            break;
        }

        if (const u16 *frame = camera.takeFrame()) {
            const int next = shown == 0 ? 1 : 0;
            image::tileImage(frame, FrameWidth, FrameHeight,
                             static_cast<u16 *>(previewTextures[next].data),
                             TextureWidth);
            C3D_TexFlush(&previewTextures[next]);
            shown = next;

            int width = 0;
            int height = 0;
            u8 *gray = quirc_begin(decoder, &width, &height);
            image::rgb565ToGray(frame, gray, FramePixels);
            quirc_end(decoder);

            std::string payload;
            if (decodeAny(decoder, payload)) {
                payload = text::trim(payload);
                if (url::isHttpUrl(payload)) {
                    found = payload;
                    status = "QR code found";
                    statusColor = ui::color::Success;
                } else {
                    status = "That QR code does not hold a web link.";
                    statusColor = ui::color::Warning;
                    noticeUntil = osGetTime() + NoticeDurationMs;
                }
            }
        }

        if (noticeUntil != 0 && osGetTime() > noticeUntil) {
            status = LookingText;
            statusColor = ui::color::Success;
            noticeUntil = 0;
        }

        ui::drawScanner(shown >= 0 ? &images[shown] : nullptr, status,
                        statusColor);
        if (!found.empty()) {
            break;
        }
    }

    for (C3D_Tex &texture : previewTextures) {
        C3D_TexDelete(&texture);
    }
    quirc_destroy(decoder);
    camera.close();

    if (!error.empty()) {
        ui::message("Camera error", error, ui::Tone::Error);
    }
    return found;
}

} // namespace scanner
