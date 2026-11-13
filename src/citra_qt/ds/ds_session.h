// Copyright AzalonDS Project
// Licensed under GPLv3 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include <QImage>
#include <QObject>
#include <QString>

#include "core/frontend/input.h"

struct azds_console;
class QOffscreenSurface;
class QOpenGLContext;

namespace AudioCore {
class Sink;
}

namespace DS {

/// One running DS/DSi game: owns the azalon_ds console, the emulation thread,
/// audio output and input devices (mapped from the current Azahar input profile).
class Session : public QObject {
    Q_OBJECT
public:
    explicit Session(QObject* parent = nullptr);
    ~Session() override;

    /// Creates the console, loads the ROM and starts emulating. Returns false with an error.
    bool Start(const QString& rom_path, QString* error);
    void Stop();

    bool IsRunning() const {
        return running;
    }
    bool IsPaused() const {
        return paused;
    }
    void SetPaused(bool pause);
    void Reset();

    /// Hold to run unthrottled.
    void SetFastForward(bool enabled) {
        fast_forward = enabled;
    }

    bool SaveState(int slot);
    bool LoadState(int slot);

    const QString& RomPath() const {
        return rom_path;
    }
    const QString& Title() const {
        return title;
    }

    /// Re-reads the video settings of Config (renderer, internal resolution...) and applies
    /// them to the running game.
    void ApplyRenderSettings();

    /// Latest frame: both screens stacked (RGB32), (256*scale)x(384*scale). Cheap shared copy.
    QImage CurrentFrame() const;

    /// Touch input in DS bottom-screen coordinates (called from the GUI thread).
    void SetTouch(bool pressed, int x, int y);

    /// Emulated frames per second over the last second.
    double Fps() const {
        return fps;
    }

signals:
    void FrameReady();
    void Stopped(); ///< emulated console powered off by itself
    void Message(const QString& text); ///< something for the status bar (any thread)

private:
    void EmuLoop();
    void OpenAudio();
    void CloseAudio();
    void LoadInputDevices();
    uint32_t PollButtons() const;
    QString StatePath(int slot) const;
    /// Emulation thread: switches the core to `renderer` (azds_renderer), creating or
    /// destroying the OpenGL context as needed. Falls back to software on failure.
    void UseRenderer(int renderer);
    void DestroyGL();

    azds_console* console = nullptr;
    std::thread emu_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> fast_forward{false};
    std::atomic<double> fps{0.0};

    /// Runs `task` on the emulation thread between frames (or inline if it isn't running)
    /// and returns its result. melonDS is not thread-safe.
    bool RunOnEmuThread(std::function<bool()> task);

    std::mutex command_lock;
    std::deque<std::packaged_task<bool()>> commands;

    mutable std::mutex frame_lock;
    QImage frame;

    // OpenGL renderers: the surface is created on the GUI thread (Qt requires it), the
    // context lives on the emulation thread and stays current there.
    std::unique_ptr<QOffscreenSurface> gl_surface;
    std::unique_ptr<QOpenGLContext> gl_context;

    std::atomic<bool> touch_pressed{false};
    std::atomic<int> touch_x{0}, touch_y{0};

    std::unique_ptr<AudioCore::Sink> sink;
    std::array<std::unique_ptr<Input::ButtonDevice>, 12> buttons;
    std::unique_ptr<Input::AnalogDevice> circle_pad;

    QString rom_path;
    QString save_path;
    QString title;
};

} // namespace DS
