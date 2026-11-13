// Copyright AzalonDS Project
// Licensed under GPLv3 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

#include <dlfcn.h>
#include <EGL/eglext.h>

#include <azalon_ds/azalon_ds.h>

#include "audio_core/sink.h"
#include "audio_core/sink_details.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "jni/ds/ds_session.h"

namespace DS {

namespace {

constexpr double kDSFrameRate = 59.8261; // 33513982 Hz / (6 * 355 * 263)

void LogFromCore(void*, azds_log_level level, const char* message) {
    switch (level) {
    case AZDS_LOG_DEBUG:
        LOG_DEBUG(Frontend, "[DS] {}", message);
        break;
    case AZDS_LOG_INFO:
        LOG_INFO(Frontend, "[DS] {}", message);
        break;
    case AZDS_LOG_WARN:
        LOG_WARNING(Frontend, "[DS] {}", message);
        break;
    default:
        LOG_ERROR(Frontend, "[DS] {}", message);
        break;
    }
}

void OnCoreStop(void*, int reason) {
    constexpr int kStopReasonExternal = 1; // melonDS StopReason::External: we stopped it
    if (reason == kStopReasonExternal) {
        return;
    }
    LOG_INFO(Frontend, "[DS] console stopped by itself (reason {})", reason);
    NotifyStopped();
}

// azalon_ds hands out 0xAARRGGBB words (B,G,R,A in memory); Android's
// RGBA/RGBX_8888 windows want R,G,B,A, so red and blue have to be swapped.
constexpr std::uint32_t ToRGBX(std::uint32_t v) {
    return 0xFF000000u | ((v & 0x00FF0000u) >> 16) | (v & 0x0000FF00u) |
           ((v & 0x000000FFu) << 16);
}

// The buffer is the output size times a whole factor (see Session::ApplyGeometry);
// anything above 1x is a nearest-neighbour upscale, so the compositor's bilinear
// scaling to the view size barely blurs the pixels.
void BlitScreen(const std::uint32_t* src, int src_w, int src_h, ANativeWindow_Buffer& buffer) {
    const int scale = std::max(1, std::min(buffer.width / src_w, buffer.height / src_h));
    auto* dst = static_cast<std::uint32_t*>(buffer.bits);

    if (scale == 1) {
        const int width = std::min(buffer.width, src_w);
        const int height = std::min(buffer.height, src_h);
        for (int y = 0; y < height; y++) {
            const std::uint32_t* in = src + static_cast<std::size_t>(y) * src_w;
            std::uint32_t* out = dst + static_cast<std::size_t>(y) * buffer.stride;
            for (int x = 0; x < width; x++) {
                out[x] = ToRGBX(in[x]);
            }
        }
        return;
    }

    const std::size_t row_bytes =
        static_cast<std::size_t>(src_w) * scale * sizeof(std::uint32_t);
    for (int y = 0; y < src_h; y++) {
        const std::uint32_t* in = src + static_cast<std::size_t>(y) * src_w;
        std::uint32_t* first =
            dst + static_cast<std::size_t>(y) * scale * static_cast<std::size_t>(buffer.stride);
        std::uint32_t* out = first;
        for (int x = 0; x < src_w; x++) {
            const std::uint32_t v = ToRGBX(in[x]);
            for (int i = 0; i < scale; i++) {
                *out++ = v;
            }
        }
        for (int i = 1; i < scale; i++) {
            std::memcpy(first + static_cast<std::size_t>(i) * buffer.stride, first, row_bytes);
        }
    }
}

// GL loader for the DS core: core GLES entry points from libGLESv3, the rest through EGL.
void* GetGLProcAddress(void*, const char* name) {
    static void* const gles = [] {
        void* lib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
        return lib ? lib : dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
    }();
    void* proc = gles ? dlsym(gles, name) : nullptr;
    if (!proc) {
        proc = reinterpret_cast<void*>(eglGetProcAddress(name));
    }
    return proc;
}

const char* OrNull(const std::string& s) {
    return s.empty() ? nullptr : s.c_str();
}

} // namespace

Session& Session::Get() {
    static Session session;
    return session;
}

Session::~Session() {
    Stop();
    std::lock_guard guard(surface_lock);
    for (auto*& window : surfaces) {
        if (window) {
            ANativeWindow_release(window);
            window = nullptr;
        }
    }
}

bool Session::Start(const StartParams& params, std::string* error) {
    Stop();

    // Wireless and internet are process-wide in the DS core.
    azds_mp_set_mode(static_cast<azds_mp_mode>(params.wireless_mode));
    azds_internet_enable(params.internet ? 1 : 0);

    OpenAudio();

    azds_config config;
    azds_config_init(&config);
    config.console_type = params.console_type == 1 ? AZDS_CONSOLE_DSI : AZDS_CONSOLE_DS;
    config.renderer = AZDS_RENDERER_SOFTWARE;
    config.threaded_renderer = params.threaded_renderer ? 1 : 0;
    config.scale_factor = params.scale_factor;
    config.gl_better_polygons = params.better_polygons ? 1 : 0;
    config.jit_enable = params.jit ? 1 : 0;
    config.direct_boot = params.direct_boot ? 1 : 0;
    config.audio_sample_rate = sink ? sink->GetNativeSampleRate() : 48000.0;
    config.data_dir = params.data_dir.c_str();
    config.ds_bios9_path = OrNull(params.ds_bios9);
    config.ds_bios7_path = OrNull(params.ds_bios7);
    config.ds_firmware_path = OrNull(params.ds_firmware);
    config.dsi_bios9_path = OrNull(params.dsi_bios9);
    config.dsi_bios7_path = OrNull(params.dsi_bios7);
    config.dsi_firmware_path = OrNull(params.dsi_firmware);
    config.dsi_nand_path = OrNull(params.dsi_nand);
    config.firmware_username = OrNull(params.nickname);
    config.firmware_language = params.language;
    config.firmware_favorite_color = params.favorite_color;

    const std::string dldi_image = params.data_dir + "dldi_sd.bin";
    if (params.dldi_enable) {
        config.dldi_image_path = dldi_image.c_str();
        config.dldi_image_size = 0; // sized automatically
        config.dldi_sync_folder = OrNull(params.dldi_sync_folder);
    }
    const std::string dsi_sd_image = params.data_dir + "dsi_sd.bin";
    config.dsi_sd_image_path = dsi_sd_image.c_str();
    config.dsi_sd_image_size = 0;

    config.callbacks.log = LogFromCore;
    config.callbacks.on_stop = OnCoreStop;

    char err[512] = {};
    console = azds_create(&config, err, sizeof(err));
    if (!console) {
        CloseAudio();
        if (error) {
            *error = err;
        }
        return false;
    }

    // No ROM = boot the console's own firmware menu (needs BIOS/firmware dumps).
    if (!params.rom_path.empty() &&
        !azds_load_rom(console, params.rom_path.c_str(), params.save_path.c_str(), err,
                       sizeof(err))) {
        azds_destroy(console);
        console = nullptr;
        CloseAudio();
        if (error) {
            *error = err;
        }
        return false;
    }

    if (!azds_start(console)) {
        azds_destroy(console);
        console = nullptr;
        CloseAudio();
        if (error) {
            *error = params.rom_path.empty() ? "The DS core could not boot the firmware."
                                             : "The DS core could not boot this game.";
        }
        return false;
    }

    keys = 0;
    touch_pressed = false;
    paused = false;
    fast_forward = false;
    running = true;
    emu_thread = std::thread([this, renderer = params.renderer] {
        if (renderer != AZDS_RENDERER_SOFTWARE) {
            UseRenderer(renderer);
        }
        EmuLoop();
        DestroyGL(); // back to software: azds_stop/destroy run on the UI thread
    });
    LOG_INFO(Frontend, "[DS] started {}",
             params.rom_path.empty() ? std::string("firmware") : params.rom_path);
    return true;
}

void Session::Stop() {
    if (running) {
        running = false;
        if (emu_thread.joinable()) {
            emu_thread.join();
        }
    }
    {
        // Fail any commands that never ran.
        std::lock_guard guard(command_lock);
        commands.clear();
    }
    CloseAudio();
    if (console) {
        azds_stop(console);
        azds_destroy(console); // flushes saves
        console = nullptr;
    }
}

void Session::SetPaused(bool pause) {
    paused = pause;
}

void Session::Reset() {
    RunOnEmuThread([this] {
        azds_reset(console);
        return true;
    });
}

bool Session::SaveState(const std::string& path) {
    return RunOnEmuThread([this, path] { return azds_save_state(console, path.c_str()) != 0; });
}

bool Session::LoadState(const std::string& path) {
    return RunOnEmuThread([this, path] { return azds_load_state(console, path.c_str()) != 0; });
}

bool Session::RunOnEmuThread(std::function<bool()> task) {
    if (!console) {
        return false;
    }
    if (!running || std::this_thread::get_id() == emu_thread.get_id()) {
        return task();
    }

    std::packaged_task<bool()> packaged(std::move(task));
    auto result = packaged.get_future();
    {
        std::lock_guard guard(command_lock);
        commands.push_back(std::move(packaged));
    }
    if (result.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        return false;
    }
    try {
        return result.get();
    } catch (const std::future_error&) {
        return false; // the session stopped before running the command
    }
}

void Session::SetSurface(Screen screen, ANativeWindow* window) {
    const auto index = static_cast<int>(screen);
    if (index < 0 || index >= kNumScreens) {
        if (window) {
            ANativeWindow_release(window);
        }
        return;
    }

    std::lock_guard guard(surface_lock);
    if (surfaces[index]) {
        ANativeWindow_release(surfaces[index]);
    }
    surfaces[index] = window;
    surface_size[index][0] = surface_size[index][1] = 0;
    ApplyGeometry(index);
}

void Session::ApplyGeometry(int index) {
    ANativeWindow* window = surfaces[index];
    if (!window) {
        return;
    }
    // A whole multiple of the core's output (native, or upscaled by the OpenGL renderer)
    // that is at least as big as the view asked for; BlitScreen fills it nearest-neighbour.
    const int wanted = AZDS_SCREEN_WIDTH * surface_scale[index];
    const int factor = std::max(1, wanted / std::max(1, output_width));
    const int width = output_width * factor;
    const int height = output_height * factor;
    if (surface_size[index][0] == width && surface_size[index][1] == height) {
        return;
    }
    ANativeWindow_setBuffersGeometry(window, width, height, WINDOW_FORMAT_RGBX_8888);
    surface_size[index][0] = width;
    surface_size[index][1] = height;
}

void Session::SetSurfaceScale(Screen screen, int scale) {
    const auto index = static_cast<int>(screen);
    if (index < 0 || index >= kNumScreens) {
        return;
    }
    scale = std::clamp(scale, 1, kMaxSurfaceScale);

    std::lock_guard guard(surface_lock);
    if (surface_scale[index] == scale) {
        return;
    }
    surface_scale[index] = scale;
    ApplyGeometry(index);
}

void Session::SetTouch(bool pressed, int x, int y) {
    touch_x = std::clamp(x, 0, AZDS_SCREEN_WIDTH - 1);
    touch_y = std::clamp(y, 0, AZDS_SCREEN_HEIGHT - 1);
    touch_pressed = pressed;
}

void Session::SetLidClosed(bool closed) {
    RunOnEmuThread([this, closed] {
        azds_set_lid_closed(console, closed ? 1 : 0);
        return true;
    });
}

void Session::PresentFrame() {
    const std::uint32_t* screens[kNumScreens] = {};
    int width = 0, height = 0;
    if (!azds_get_framebuffers_ex(console, &screens[0], &screens[1], &width, &height)) {
        return;
    }

    std::lock_guard guard(surface_lock);
    if (width != output_width || height != output_height) {
        output_width = width;
        output_height = height;
        for (int i = 0; i < kNumScreens; i++) {
            ApplyGeometry(i);
        }
    }
    for (int i = 0; i < kNumScreens; i++) {
        ANativeWindow* window = surfaces[i];
        if (!window || !screens[i]) {
            continue;
        }
        ANativeWindow_Buffer buffer;
        if (ANativeWindow_lock(window, &buffer, nullptr) != 0) {
            continue;
        }
        BlitScreen(screens[i], width, height, buffer);
        ANativeWindow_unlockAndPost(window);
    }
}

void Session::UseRenderer(int renderer) {
    if (renderer == AZDS_RENDERER_SOFTWARE) {
        DestroyGL();
        return;
    }
    if (azds_get_renderer(console) == renderer) {
        return;
    }
    std::string error;
    if (egl_context == EGL_NO_CONTEXT && !CreateGL(&error)) {
        LOG_ERROR(Frontend, "[DS] {}", error);
        NotifyMessage("DS: " + error + " Using the software renderer.");
        DestroyGL();
        return;
    }
    char err[512] = {};
    if (!azds_gl_attach(console, static_cast<azds_renderer>(renderer), GetGLProcAddress, nullptr,
                        err, sizeof(err))) {
        LOG_ERROR(Frontend, "[DS] {}", err);
        NotifyMessage(std::string("DS: ") + err);
        DestroyGL();
    }
}

bool Session::CreateGL(std::string* error) {
#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif
#ifndef EGL_CONTEXT_MINOR_VERSION_KHR
#define EGL_CONTEXT_MINOR_VERSION_KHR 0x30FB
#endif
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display == EGL_NO_DISPLAY || !eglInitialize(egl_display, nullptr, nullptr)) {
        egl_display = EGL_NO_DISPLAY;
        *error = "Could not open the EGL display.";
        return false;
    }
    const EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE,        8,                      EGL_GREEN_SIZE,   8,
        EGL_BLUE_SIZE,       8,                      EGL_ALPHA_SIZE,   8,
        EGL_NONE};
    EGLConfig config = nullptr;
    EGLint num_configs = 0;
    if (!eglChooseConfig(egl_display, config_attribs, &config, 1, &num_configs) ||
        num_configs < 1) {
        *error = "No OpenGL ES 3 configuration available.";
        return false;
    }
    // The core draws into its own framebuffers; the pbuffer only makes the context current.
    const EGLint pbuffer_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    egl_surface = eglCreatePbufferSurface(egl_display, config, pbuffer_attribs);
    if (egl_surface == EGL_NO_SURFACE) {
        *error = "Could not create an EGL pbuffer.";
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint es32_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_MINOR_VERSION_KHR,
                                   2, EGL_NONE};
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, es32_attribs);
    if (egl_context == EGL_NO_CONTEXT) {
        // Without EGL_KHR_create_context drivers still hand out their newest 3.x.
        const EGLint es3_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, es3_attribs);
    }
    if (egl_context == EGL_NO_CONTEXT) {
        *error = "Could not create an OpenGL ES 3 context.";
        return false;
    }
    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        *error = "Could not make the OpenGL ES context current.";
        return false;
    }
    return true;
}

void Session::DestroyGL() {
    if (console) {
        azds_gl_detach(console); // frees the GL renderer while its context is current
    }
    if (egl_display == EGL_NO_DISPLAY) {
        return;
    }
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display, egl_context);
        egl_context = EGL_NO_CONTEXT;
    }
    if (egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(egl_display, egl_surface);
        egl_surface = EGL_NO_SURFACE;
    }
    // No eglTerminate: the display is shared with the 3DS renderer.
    egl_display = EGL_NO_DISPLAY;
}

void Session::EmuLoop() {
    using clock = std::chrono::steady_clock;
    const auto frame_time = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(1.0 / kDSFrameRate));

    auto next_frame = clock::now();
    auto fps_start = clock::now();
    int fps_frames = 0;
    bool was_touching = false;

    while (running) {
        // Commands from the UI thread (reset, save states...).
        for (;;) {
            std::packaged_task<bool()> task;
            {
                std::lock_guard guard(command_lock);
                if (commands.empty()) {
                    break;
                }
                task = std::move(commands.front());
                commands.pop_front();
            }
            task();
        }

        if (paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            next_frame = clock::now();
            continue;
        }

        azds_set_keys(console, keys.load());
        if (touch_pressed) {
            azds_touch(console, touch_x.load(), touch_y.load());
            was_touching = true;
        } else if (was_touching) {
            azds_release_touch(console);
            was_touching = false;
        }

        if (azds_run_frame(console) != 0) { // 0: the frame went to compiling GL shaders
            PresentFrame();
        }

        fps_frames++;
        const auto now = clock::now();
        if (now - fps_start >= std::chrono::seconds(1)) {
            fps = fps_frames / std::chrono::duration<double>(now - fps_start).count();
            fps_frames = 0;
            fps_start = now;
        }

        if (fast_forward) {
            next_frame = clock::now();
            continue;
        }
        next_frame += frame_time;
        if (next_frame < now - std::chrono::milliseconds(100)) {
            next_frame = now; // we fell far behind: don't try to catch up
        }
        std::this_thread::sleep_until(next_frame);
    }
}

void Session::OpenAudio() {
    CloseAudio();
    const auto& details = AudioCore::GetSinkDetails(Settings::values.output_type.GetValue());
    sink = details.create_sink(Settings::values.output_device.GetValue());
    if (!sink) {
        LOG_ERROR(Frontend, "[DS] could not open audio output");
        return;
    }
    sink->SetCallback([this](s16* buffer, std::size_t num_frames) {
        int done = 0;
        if (console && running && !paused) {
            done = azds_read_audio(console, buffer, static_cast<int>(num_frames));
        }
        if (static_cast<std::size_t>(done) < num_frames) {
            std::memset(buffer + static_cast<std::size_t>(done) * 2, 0,
                        (num_frames - static_cast<std::size_t>(done)) * 2 * sizeof(s16));
        }
        const float volume = Settings::values.volume.GetValue();
        if (volume < 1.0f) {
            for (std::size_t i = 0; i < num_frames * 2; i++) {
                buffer[i] = static_cast<s16>(buffer[i] * volume);
            }
        }
    });
}

void Session::CloseAudio() {
    if (sink) {
        sink->SetCallback([](s16* buffer, std::size_t num_frames) {
            std::memset(buffer, 0, num_frames * 2 * sizeof(s16));
        });
        sink.reset();
    }
}

} // namespace DS
