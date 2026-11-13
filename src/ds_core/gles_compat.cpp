// Copyright AzalonDS Project
// Licensed under GPLv3 or any later version
// Refer to the license.txt file included.

// Runs melonDS's desktop OpenGL renderer on OpenGL ES 3.2 (Android).
//
// melonDS calls OpenGL through glad's function pointers (glad_glXxx). glad is compiled into
// this library, so on an ES context we can replace the few entry points whose desktop
// behaviour ES lacks with small adapters, without touching melonDS:
//   * glShaderSource: GLSL 1.40 -> GLSL ES 3.00 (version line, default precisions, explicit
//     fragment output locations, because ES has no glBindFragDataLocation);
//   * GL_UNSIGNED_SHORT_1_5_5_5_REV (the DS's native colour layout, missing in ES): pixel
//     data is converted to/from GL_UNSIGNED_SHORT_5_5_5_1 on upload and read back;
//   * glReadPixels with GL_BGRA (used by azds_get_framebuffers_ex);
//   * desktop-only calls with an ES equivalent (glDrawBuffer, glClearDepth, glDepthRange,
//     glMapBuffer) or none needed (glBindFragDataLocation).
// The shaders themselves are written to compile as both GLSL 1.40 and GLSL ES 3.00.

#ifdef AZDS_HAVE_OPENGL

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "GPU_OpenGL.h"

#include "ds_console.h"

namespace {

PFNGLSHADERSOURCEPROC real_ShaderSource = nullptr;
PFNGLTEXIMAGE2DPROC real_TexImage2D = nullptr;
PFNGLTEXIMAGE3DPROC real_TexImage3D = nullptr;
PFNGLTEXSUBIMAGE2DPROC real_TexSubImage2D = nullptr;
PFNGLTEXSUBIMAGE3DPROC real_TexSubImage3D = nullptr;
PFNGLREADPIXELSPROC real_ReadPixels = nullptr;
PFNGLMAPBUFFERRANGEPROC real_MapBufferRange = nullptr;
PFNGLDRAWBUFFERSPROC real_DrawBuffers = nullptr;
PFNGLCLEARDEPTHFPROC real_ClearDepthf = nullptr;
PFNGLDEPTHRANGEFPROC real_DepthRangef = nullptr;

// ---------------------------------------------------------------------------------------
// Shaders

constexpr const char* kESHeader = "#version 300 es\n"
                                  "precision highp float;\n"
                                  "precision highp int;\n"
                                  "precision highp sampler2D;\n"
                                  "precision highp sampler2DArray;\n"
                                  "precision highp usampler2D;\n"
                                  "precision highp isampler2D;\n"
                                  "precision highp usampler2DArray;\n"
                                  "precision highp isampler2DArray;\n";

// Fragment outputs of melonDS's shaders and the locations its C++ code binds them to
// (OpenGL::CompileVertexFragmentProgram's fragmentOutAttrs).
struct FragOutput {
    const char* name;
    int location;
};
constexpr FragOutput kFragOutputs[] = {
    {"oColor", 0}, {"oTopColor", 0}, {"oBottomColor", 1}, {"oAttr", 1}, {"oFlags", 1},
};

std::string ToGLSLES(std::string source) {
    constexpr const char kDesktopVersion[] = "#version 140";
    if (source.compare(0, sizeof(kDesktopVersion) - 1, kDesktopVersion) != 0)
        return source; // not one of ours (or already ES)
    const size_t eol = source.find('\n');
    source.replace(0, eol == std::string::npos ? source.size() : eol + 1, kESHeader);

    for (const FragOutput& output : kFragOutputs) {
        const std::string decl = std::string("out vec4 ") + output.name + ";";
        size_t pos = 0;
        while ((pos = source.find(decl, pos)) != std::string::npos) {
            const bool word_start = pos == 0 || source[pos - 1] == '\n' ||
                                    source[pos - 1] == ' ' || source[pos - 1] == '\t';
            if (word_start) {
                const std::string layout =
                    "layout(location = " + std::to_string(output.location) + ") ";
                source.insert(pos, layout);
                pos += layout.size();
            }
            pos += decl.size();
        }
    }
    return source;
}

void APIENTRY ShaderSourceES(GLuint shader, GLsizei count, const GLchar* const* strings,
                             const GLint* lengths) {
    std::string source;
    for (GLsizei i = 0; i < count; i++) {
        if (!strings[i])
            continue;
        if (lengths && lengths[i] >= 0)
            source.append(strings[i], static_cast<size_t>(lengths[i]));
        else
            source.append(strings[i]);
    }
    source = ToGLSLES(std::move(source));
    const GLchar* text = source.c_str();
    real_ShaderSource(shader, 1, &text, nullptr);
}

void APIENTRY BindFragDataLocationES(GLuint, GLuint, const GLchar*) {
    // ES has no equivalent: ToGLSLES() writes the locations into the shaders instead.
}

// ---------------------------------------------------------------------------------------
// 1555 pixel data

// DS colour (R bits 0-4, G 5-9, B 10-14, A 15) -> GL_UNSIGNED_SHORT_5_5_5_1 (R 11-15,
// G 6-10, B 1-5, A 0).
inline std::uint16_t Rev1555To5551(std::uint16_t v) {
    return static_cast<std::uint16_t>(((v & 0x1F) << 11) | (((v >> 5) & 0x1F) << 6) |
                                      (((v >> 10) & 0x1F) << 1) | (v >> 15));
}

size_t UnpackedBytes(GLsizei width, GLsizei height, GLsizei depth) {
    // Default GL_UNPACK_ALIGNMENT (4); melonDS never changes the unpack state.
    const size_t row = (static_cast<size_t>(width) * 2 + 3) & ~size_t{3};
    return row * static_cast<size_t>(height) * static_cast<size_t>(depth);
}

// Converts `pixels` if needed; returns the pointer and type to hand to ES.
const void* Convert1555(GLenum& type, const void* pixels, GLsizei width, GLsizei height,
                        GLsizei depth, std::vector<std::uint16_t>& scratch) {
    if (type != GL_UNSIGNED_SHORT_1_5_5_5_REV)
        return pixels;
    type = GL_UNSIGNED_SHORT_5_5_5_1;
    if (!pixels)
        return nullptr; // allocation only
    const size_t count = UnpackedBytes(width, height, depth) / 2;
    scratch.resize(count);
    const auto* in = static_cast<const std::uint16_t*>(pixels);
    for (size_t i = 0; i < count; i++)
        scratch[i] = Rev1555To5551(in[i]);
    return scratch.data();
}

void APIENTRY TexImage2DES(GLenum target, GLint level, GLint internalformat, GLsizei width,
                           GLsizei height, GLint border, GLenum format, GLenum type,
                           const void* pixels) {
    std::vector<std::uint16_t> scratch;
    pixels = Convert1555(type, pixels, width, height, 1, scratch);
    real_TexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}

void APIENTRY TexImage3DES(GLenum target, GLint level, GLint internalformat, GLsizei width,
                           GLsizei height, GLsizei depth, GLint border, GLenum format,
                           GLenum type, const void* pixels) {
    std::vector<std::uint16_t> scratch;
    pixels = Convert1555(type, pixels, width, height, depth, scratch);
    real_TexImage3D(target, level, internalformat, width, height, depth, border, format, type,
                    pixels);
}

void APIENTRY TexSubImage2DES(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                              GLsizei width, GLsizei height, GLenum format, GLenum type,
                              const void* pixels) {
    std::vector<std::uint16_t> scratch;
    pixels = Convert1555(type, pixels, width, height, 1, scratch);
    real_TexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
}

void APIENTRY TexSubImage3DES(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                              GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                              GLenum format, GLenum type, const void* pixels) {
    std::vector<std::uint16_t> scratch;
    pixels = Convert1555(type, pixels, width, height, depth, scratch);
    real_TexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format,
                       type, pixels);
}

inline std::uint16_t RGBA8ToRev1555(const std::uint8_t* p) {
    const auto to5 = [](std::uint8_t c) { return static_cast<std::uint16_t>((c * 31 + 127) / 255); };
    return static_cast<std::uint16_t>(to5(p[0]) | (to5(p[1]) << 5) | (to5(p[2]) << 10) |
                                      ((p[3] >= 128 ? 1 : 0) << 15));
}

void APIENTRY ReadPixelsES(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                           GLenum type, void* pixels) {
    // ES always supports RGBA / UNSIGNED_BYTE: read that and convert.
    if (format == GL_RGBA && type == GL_UNSIGNED_SHORT_1_5_5_5_REV) {
        std::vector<std::uint8_t> rgba(static_cast<size_t>(width) * height * 4);
        GLint pack = 4;
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        real_ReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glPixelStorei(GL_PACK_ALIGNMENT, pack);
        const size_t row = (static_cast<size_t>(width) * 2 + pack - 1) / pack * pack;
        for (GLsizei j = 0; j < height; j++) {
            auto* out = reinterpret_cast<std::uint16_t*>(static_cast<std::uint8_t*>(pixels) +
                                                         row * static_cast<size_t>(j));
            const std::uint8_t* in = rgba.data() + static_cast<size_t>(j) * width * 4;
            for (GLsizei i = 0; i < width; i++)
                out[i] = RGBA8ToRev1555(in + static_cast<size_t>(i) * 4);
        }
        return;
    }
    if (format == GL_BGRA && type == GL_UNSIGNED_BYTE) {
        // 4-byte pixels: rows are tightly packed for any pack alignment.
        real_ReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        auto* p = static_cast<std::uint8_t*>(pixels);
        const size_t count = static_cast<size_t>(width) * height;
        for (size_t i = 0; i < count; i++, p += 4)
            std::swap(p[0], p[2]);
        return;
    }
    real_ReadPixels(x, y, width, height, format, type, pixels);
}

// ---------------------------------------------------------------------------------------
// Desktop-only entry points

void APIENTRY DrawBufferES(GLenum buf) {
    real_DrawBuffers(1, &buf);
}

void APIENTRY ClearDepthES(GLdouble depth) {
    real_ClearDepthf(static_cast<GLfloat>(depth));
}

void APIENTRY DepthRangeES(GLdouble n, GLdouble f) {
    real_DepthRangef(static_cast<GLfloat>(n), static_cast<GLfloat>(f));
}

void* APIENTRY MapBufferES(GLenum target, GLenum access) {
    GLint size = 0;
    glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
    GLbitfield bits = 0;
    switch (access) {
    case GL_READ_ONLY:
        bits = GL_MAP_READ_BIT;
        break;
    case GL_READ_WRITE:
        bits = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
        break;
    default: // GL_WRITE_ONLY
        bits = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT;
        break;
    }
    return real_MapBufferRange(target, 0, size, bits);
}

template <typename T>
void LoadIfMissing(T& pointer, const char* name, void* (*load)(const char*)) {
    if (!pointer)
        pointer = reinterpret_cast<T>(load(name));
}

} // namespace

namespace AzalonDS {

bool InstallGLESCompat(void* (*load)(const char*), std::string* error) {
    // glad treats an ES 3.2 context as "GL 3.2" and skips everything newer, even what ES 3.2
    // has under the same name. Load those by hand.
    LoadIfMissing(glad_glTexStorage2D, "glTexStorage2D", load);
    LoadIfMissing(glad_glTexStorage3D, "glTexStorage3D", load);
    LoadIfMissing(glad_glProgramBinary, "glProgramBinary", load);
    LoadIfMissing(glad_glGetProgramBinary, "glGetProgramBinary", load);
    LoadIfMissing(glad_glProgramParameteri, "glProgramParameteri", load);
    LoadIfMissing(glad_glClearDepthf, "glClearDepthf", load);
    LoadIfMissing(glad_glDepthRangef, "glDepthRangef", load);
    LoadIfMissing(glad_glColorMaski, "glColorMaski", load);
    LoadIfMissing(glad_glFramebufferTexture, "glFramebufferTexture", load);

    real_ShaderSource = glad_glShaderSource;
    real_TexImage2D = glad_glTexImage2D;
    real_TexImage3D = glad_glTexImage3D;
    real_TexSubImage2D = glad_glTexSubImage2D;
    real_TexSubImage3D = glad_glTexSubImage3D;
    real_ReadPixels = glad_glReadPixels;
    real_MapBufferRange = glad_glMapBufferRange;
    real_DrawBuffers = glad_glDrawBuffers;
    real_ClearDepthf = glad_glClearDepthf;
    real_DepthRangef = glad_glDepthRangef;

    const struct {
        const void* pointer;
        const char* name;
    } required[] = {
        {reinterpret_cast<const void*>(real_ShaderSource), "glShaderSource"},
        {reinterpret_cast<const void*>(real_TexImage3D), "glTexImage3D"},
        {reinterpret_cast<const void*>(real_MapBufferRange), "glMapBufferRange"},
        {reinterpret_cast<const void*>(real_DrawBuffers), "glDrawBuffers"},
        {reinterpret_cast<const void*>(real_ClearDepthf), "glClearDepthf"},
        {reinterpret_cast<const void*>(real_DepthRangef), "glDepthRangef"},
        {reinterpret_cast<const void*>(glad_glColorMaski), "glColorMaski"},
        {reinterpret_cast<const void*>(glad_glFramebufferTexture), "glFramebufferTexture"},
        {reinterpret_cast<const void*>(glad_glTexStorage3D), "glTexStorage3D"},
    };
    for (const auto& entry : required) {
        if (!entry.pointer) {
            if (error)
                *error = std::string("The OpenGL ES driver lacks ") + entry.name +
                         " (needs OpenGL ES 3.2).";
            return false;
        }
    }

    glad_glShaderSource = ShaderSourceES;
    glad_glBindFragDataLocation = BindFragDataLocationES;
    glad_glTexImage2D = TexImage2DES;
    glad_glTexImage3D = TexImage3DES;
    glad_glTexSubImage2D = TexSubImage2DES;
    glad_glTexSubImage3D = TexSubImage3DES;
    glad_glReadPixels = ReadPixelsES;
    glad_glDrawBuffer = DrawBufferES;
    glad_glClearDepth = ClearDepthES;
    glad_glDepthRange = DepthRangeES;
    glad_glMapBuffer = MapBufferES;
    return true;
}

} // namespace AzalonDS

#endif // AZDS_HAVE_OPENGL
