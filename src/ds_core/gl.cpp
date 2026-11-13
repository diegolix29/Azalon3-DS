// Copyright AzalonDS Project
// Licensed under GPLv3 or any later version
// Refer to the license.txt file included.

// Renderer selection: melonDS's software renderer, or its OpenGL / OpenGL compute
// renderers running in a context owned by the frontend. The GL output is read back into
// RAM so every frontend keeps drawing plain pixel buffers, only bigger.

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include "GPU.h"
#include "GPU_Soft.h"
#include "NDS.h"

#ifdef AZDS_HAVE_OPENGL
#include "GPU_OpenGL.h"
#endif

#include "ds_console.h"

using namespace melonDS;
using AzalonDS::LogMessage;
using AzalonDS::SetError;

namespace {

int ScreenScale(const azds_console& console) {
    return console.active_renderer == AZDS_RENDERER_SOFTWARE ? 1 : console.scale_factor;
}

#ifdef AZDS_HAVE_OPENGL

// glad's loader callback has no user pointer. Only set during azds_gl_attach().
azds_gl_get_proc_address g_get_proc = nullptr;
void* g_get_proc_userdata = nullptr;

void* LoadGLProc(const char* name) {
    return g_get_proc ? g_get_proc(g_get_proc_userdata, name) : nullptr;
}

void FreeReadback(azds_console& console) {
    if (console.gl_read_fbo) {
        glDeleteFramebuffers(1, &console.gl_read_fbo);
        console.gl_read_fbo = 0;
    }
    console.gl_top.clear();
    console.gl_top.shrink_to_fit();
    console.gl_bottom.clear();
    console.gl_bottom.shrink_to_fit();
}

bool ReadBackGL(azds_console& console, int width, int height) {
    void* top = nullptr;
    void* bottom = nullptr;
    console.nds->GPU.GetFramebuffers(&top, &bottom);
    if (!top)
        return false;
    // GLRenderer returns a pointer to its front output texture: a 2-layer array texture
    // (layer 0 = top screen, layer 1 = bottom screen), row 0 = first scanline.
    const GLuint texture = *static_cast<const GLuint*>(top);

    const size_t pixels = static_cast<size_t>(width) * height;
    console.gl_top.resize(pixels);
    console.gl_bottom.resize(pixels);

    if (!console.gl_read_fbo)
        glGenFramebuffers(1, &console.gl_read_fbo);

    GLint old_read_fbo = 0, old_pack_alignment = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read_fbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack_alignment);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, console.gl_read_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    u32* const targets[2] = {console.gl_top.data(), console.gl_bottom.data()};
    for (int layer = 0; layer < 2; layer++) {
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, layer);
        // BGRA bytes = little-endian 0xAARRGGBB, the software renderer's format.
        glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, targets[layer]);
    }

    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_read_fbo));
    glPixelStorei(GL_PACK_ALIGNMENT, old_pack_alignment);
    return true;
}

#endif // AZDS_HAVE_OPENGL

} // namespace

extern "C" {

AZDS_API int azds_gl_supported(void) {
#ifdef AZDS_HAVE_OPENGL
    return 1;
#else
    return 0;
#endif
}

AZDS_API int azds_gl_attach(azds_console* console, azds_renderer renderer,
                            azds_gl_get_proc_address get_proc_address, void* userdata,
                            char* err, size_t err_len) {
    if (!console || !console->nds || !get_proc_address ||
        (renderer != AZDS_RENDERER_OPENGL && renderer != AZDS_RENDERER_OPENGL_COMPUTE)) {
        SetError(err, err_len, "azds_gl_attach: bad arguments");
        return 0;
    }
#ifdef AZDS_HAVE_OPENGL
    if (console->active_renderer == renderer)
        return 1;
    if (console->active_renderer != AZDS_RENDERER_SOFTWARE)
        azds_gl_detach(console);

    struct LoaderScope {
        LoaderScope(azds_gl_get_proc_address proc, void* data) {
            g_get_proc = proc;
            g_get_proc_userdata = data;
        }
        ~LoaderScope() {
            g_get_proc = nullptr;
            g_get_proc_userdata = nullptr;
        }
    } loader_scope(get_proc_address, userdata);

    if (!gladLoadGLLoader(LoadGLProc) || !glGetString) {
        SetError(err, err_len, "Could not load the OpenGL functions (is a context current?)");
        return 0;
    }

    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const auto* gpu = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    LogMessage(AZDS_LOG_INFO, std::string("OpenGL ") + (version ? version : "?") + " on " +
                                  (gpu ? gpu : "?"));
    const bool gles = version && std::strncmp(version, "OpenGL ES", 9) == 0;

    const bool compute = renderer == AZDS_RENDERER_OPENGL_COMPUTE;
    if (gles) {
        // glad reports an ES context's version as GLAD_GL_VERSION_<major>_<minor>.
        if (!GLAD_GL_VERSION_3_2) {
            SetError(err, err_len, "The DS OpenGL renderer needs OpenGL ES 3.2 or newer.");
            return 0;
        }
        if (compute) {
            SetError(err, err_len,
                     "The DS compute shader renderer needs desktop OpenGL 4.3; use the "
                     "OpenGL renderer on OpenGL ES.");
            return 0;
        }
        std::string why;
        if (!AzalonDS::InstallGLESCompat(LoadGLProc, &why)) {
            SetError(err, err_len, why);
            return 0;
        }
    } else {
        if (!GLAD_GL_VERSION_3_2) {
            SetError(err, err_len, "The DS OpenGL renderer needs OpenGL 3.2 or newer.");
            return 0;
        }
        if (compute && !GLAD_GL_VERSION_4_3) {
            SetError(err, err_len, "The DS compute shader renderer needs OpenGL 4.3 or newer.");
            return 0;
        }
    }

    // GPU::SetRenderer falls back to the software renderer by itself if Init() fails.
    console->nds->SetRenderer(std::make_unique<GLRenderer>(*console->nds, compute));
    if (!dynamic_cast<GLRenderer*>(&console->nds->GetRenderer())) {
        console->active_renderer = AZDS_RENDERER_SOFTWARE;
        AzalonDS::ApplyRenderSettings(*console);
        SetError(err, err_len,
                 "The DS OpenGL renderer could not be started (shader compilation failed). "
                 "Using the software renderer.");
        return 0;
    }
    console->active_renderer = renderer;
    AzalonDS::ApplyRenderSettings(*console);
    LogMessage(AZDS_LOG_INFO, std::string("Using the ") + (compute ? "compute" : "OpenGL") +
                                  " renderer at " + std::to_string(console->scale_factor) +
                                  "x");
    return 1;
#else
    (void)userdata;
    SetError(err, err_len, "This build of the DS core has no OpenGL renderer.");
    return 0;
#endif
}

AZDS_API void azds_gl_detach(azds_console* console) {
    if (!console || !console->nds || console->active_renderer == AZDS_RENDERER_SOFTWARE)
        return;
#ifdef AZDS_HAVE_OPENGL
    FreeReadback(*console);
#endif
    // Destroys the GL renderer (and its GL objects) while the context is still current.
    console->nds->SetRenderer(std::make_unique<SoftRenderer>(*console->nds));
    console->active_renderer = AZDS_RENDERER_SOFTWARE;
    AzalonDS::ApplyRenderSettings(*console);
}

AZDS_API azds_renderer azds_get_renderer(const azds_console* console) {
    return console ? console->active_renderer : AZDS_RENDERER_SOFTWARE;
}

AZDS_API void azds_set_render_settings(azds_console* console, int threaded_renderer,
                                       int scale_factor, int gl_better_polygons,
                                       int gl_hires_coordinates) {
    if (!console || !console->nds)
        return;
    console->threaded_renderer = threaded_renderer != 0;
    console->scale_factor = std::clamp(scale_factor, 1, AZDS_MAX_SCALE_FACTOR);
    console->gl_better_polygons = gl_better_polygons != 0;
    console->gl_hires_coordinates = gl_hires_coordinates != 0;
    AzalonDS::ApplyRenderSettings(*console);
}

AZDS_API int azds_get_framebuffers_ex(azds_console* console, const uint32_t** top,
                                      const uint32_t** bottom, int* width, int* height) {
    if (!console || !console->nds)
        return 0;
    const int scale = ScreenScale(*console);
    const int w = AZDS_SCREEN_WIDTH * scale, h = AZDS_SCREEN_HEIGHT * scale;

    const uint32_t* t = nullptr;
    const uint32_t* b = nullptr;
    if (console->active_renderer == AZDS_RENDERER_SOFTWARE) {
        void* soft_top = nullptr;
        void* soft_bottom = nullptr;
        if (!console->nds->GPU.GetFramebuffers(&soft_top, &soft_bottom))
            return 0;
        t = static_cast<const uint32_t*>(soft_top);
        b = static_cast<const uint32_t*>(soft_bottom);
    } else {
#ifdef AZDS_HAVE_OPENGL
        if (!ReadBackGL(*console, w, h))
            return 0;
        t = console->gl_top.data();
        b = console->gl_bottom.data();
#else
        return 0;
#endif
    }
    if (!t || !b)
        return 0;
    if (top)
        *top = t;
    if (bottom)
        *bottom = b;
    if (width)
        *width = w;
    if (height)
        *height = h;
    return 1;
}

} // extern "C"
