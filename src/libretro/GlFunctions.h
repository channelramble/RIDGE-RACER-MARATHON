// Minimal OpenGL 3.2 core / OpenGL ES 3.0 function table, loaded through
// SDL_GL_GetProcAddress. Types and enums are declared here (Khronos values and
// spelling) so no platform GL header or loader library is needed.
#pragma once

#include <cstddef>
#include <string>

#if defined(_WIN32) && !defined(_WIN64)
#define RRM_GLAPIENTRY __stdcall
#else
#define RRM_GLAPIENTRY
#endif

namespace rrm::lr {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLubyte = unsigned char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

}  // namespace rrm::lr

#define GL_NO_ERROR                       0
#define GL_FALSE                          0
#define GL_TRUE                           1
#define GL_TRIANGLE_STRIP                 0x0005
#define GL_DEPTH_BUFFER_BIT               0x00000100
#define GL_STENCIL_BUFFER_BIT             0x00000400
#define GL_COLOR_BUFFER_BIT               0x00004000
#define GL_FRONT_AND_BACK                 0x0408
#define GL_CCW                            0x0901
#define GL_CULL_FACE                      0x0B44
#define GL_DEPTH_TEST                     0x0B71
#define GL_STENCIL_TEST                   0x0B90
#define GL_VIEWPORT                       0x0BA2
#define GL_DITHER                         0x0BD0
#define GL_BLEND                          0x0BE2
#define GL_SCISSOR_TEST                   0x0C11
#define GL_UNPACK_ROW_LENGTH              0x0CF2
#define GL_UNPACK_SKIP_ROWS               0x0CF3
#define GL_UNPACK_SKIP_PIXELS             0x0CF4
#define GL_UNPACK_ALIGNMENT               0x0CF5
#define GL_PACK_ROW_LENGTH                0x0D02
#define GL_PACK_SKIP_ROWS                 0x0D03
#define GL_PACK_SKIP_PIXELS               0x0D04
#define GL_PACK_ALIGNMENT                 0x0D05
#define GL_MAX_TEXTURE_SIZE               0x0D33
#define GL_TEXTURE_2D                     0x0DE1
#define GL_UNSIGNED_BYTE                  0x1401
#define GL_RGB                            0x1907
#define GL_RGBA                           0x1908
#define GL_FILL                           0x1B02
#define GL_VENDOR                         0x1F00
#define GL_RENDERER                       0x1F01
#define GL_VERSION                        0x1F02
#define GL_NEAREST                        0x2600
#define GL_LINEAR                         0x2601
#define GL_TEXTURE_MAG_FILTER             0x2800
#define GL_TEXTURE_MIN_FILTER             0x2801
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803
#define GL_POLYGON_OFFSET_FILL            0x8037
#define GL_RGBA8                          0x8058
#define GL_TEXTURE_BINDING_2D             0x8069
#define GL_UNPACK_SKIP_IMAGES             0x806D
#define GL_UNPACK_IMAGE_HEIGHT            0x806E
#define GL_SAMPLE_ALPHA_TO_COVERAGE       0x809E
#define GL_SAMPLE_COVERAGE                0x80A0
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_DEPTH_COMPONENT16              0x81A5
#define GL_DEPTH_COMPONENT24              0x81A6
#define GL_DEPTH_STENCIL_ATTACHMENT       0x821A
#define GL_MAJOR_VERSION                  0x821B
#define GL_MINOR_VERSION                  0x821C
#define GL_UNSIGNED_SHORT_5_6_5           0x8363
#define GL_TEXTURE0                       0x84C0
#define GL_TEXTURE1                       0x84C1
#define GL_TEXTURE2                       0x84C2
#define GL_RED                            0x1903
#define GL_RG                             0x8227
#define GL_R8                             0x8229
#define GL_RG16F                          0x822F
#define GL_FLOAT                          0x1406
#define GL_HALF_FLOAT                     0x140B
#define GL_ACTIVE_TEXTURE                 0x84E0
#define GL_MAX_RENDERBUFFER_SIZE          0x84E8
#define GL_VERTEX_ARRAY_BINDING           0x85B5
#define GL_ARRAY_BUFFER                   0x8892
#define GL_PIXEL_PACK_BUFFER              0x88EB
#define GL_PIXEL_UNPACK_BUFFER            0x88EC
#define GL_PIXEL_PACK_BUFFER_BINDING      0x88ED
#define GL_PIXEL_UNPACK_BUFFER_BINDING    0x88EF
#define GL_DEPTH24_STENCIL8               0x88F0
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_SHADING_LANGUAGE_VERSION       0x8B8C
#define GL_RASTERIZER_DISCARD             0x8C89
#define GL_FRAMEBUFFER_BINDING            0x8CA6
#define GL_RENDERBUFFER_BINDING           0x8CA7
#define GL_READ_FRAMEBUFFER               0x8CA8
#define GL_DRAW_FRAMEBUFFER               0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING       0x8CAA
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_DEPTH_ATTACHMENT               0x8D00
#define GL_FRAMEBUFFER                    0x8D40
#define GL_RENDERBUFFER                   0x8D41
#define GL_PRIMITIVE_RESTART_FIXED_INDEX  0x8D69
#define GL_FRAMEBUFFER_SRGB               0x8DB9
#define GL_PRIMITIVE_RESTART              0x8F9D
#define GL_CONTEXT_COMPATIBILITY_PROFILE_BIT 0x00000002
#define GL_CONTEXT_PROFILE_MASK           0x9126

// X(return type, name without "gl" prefix, parameter list)
#define RRM_GL_REQUIRED(X)                                                                   \
    X(GLenum, GetError, (void))                                                              \
    X(void, GetIntegerv, (GLenum pname, GLint * data))                                       \
    X(const GLubyte*, GetString, (GLenum name))                                              \
    X(void, Enable, (GLenum cap))                                                            \
    X(void, Disable, (GLenum cap))                                                           \
    X(void, Viewport, (GLint x, GLint y, GLsizei w, GLsizei h))                              \
    X(void, Scissor, (GLint x, GLint y, GLsizei w, GLsizei h))                               \
    X(void, ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                        \
    X(void, Clear, (GLbitfield mask))                                                        \
    X(void, ColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                 \
    X(void, DepthMask, (GLboolean flag))                                                     \
    X(void, StencilMask, (GLuint mask))                                                      \
    X(void, FrontFace, (GLenum mode))                                                        \
    X(void, PixelStorei, (GLenum pname, GLint param))                                        \
    X(void, ReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, \
                         void* pixels))                                                      \
    X(void, Finish, (void))                                                                  \
    X(void, GenTextures, (GLsizei n, GLuint * textures))                                     \
    X(void, DeleteTextures, (GLsizei n, const GLuint* textures))                             \
    X(void, BindTexture, (GLenum target, GLuint texture))                                    \
    X(void, ActiveTexture, (GLenum texture))                                                 \
    X(void, TexParameteri, (GLenum target, GLenum pname, GLint param))                       \
    X(void, TexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei w,        \
                         GLsizei h, GLint border, GLenum format, GLenum type,                \
                         const void* pixels))                                                \
    X(void, TexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w,         \
                            GLsizei h, GLenum format, GLenum type, const void* pixels))      \
    X(void, GenFramebuffers, (GLsizei n, GLuint * ids))                                      \
    X(void, DeleteFramebuffers, (GLsizei n, const GLuint* ids))                              \
    X(void, BindFramebuffer, (GLenum target, GLuint fbo))                                    \
    X(void, FramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget,       \
                                   GLuint texture, GLint level))                             \
    X(void, FramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rbtarget,     \
                                      GLuint rb))                                            \
    X(GLenum, CheckFramebufferStatus, (GLenum target))                                       \
    X(void, GenRenderbuffers, (GLsizei n, GLuint * ids))                                     \
    X(void, DeleteRenderbuffers, (GLsizei n, const GLuint* ids))                             \
    X(void, BindRenderbuffer, (GLenum target, GLuint rb))                                    \
    X(void, RenderbufferStorage, (GLenum target, GLenum format, GLsizei w, GLsizei h))       \
    X(GLuint, CreateShader, (GLenum type))                                                   \
    X(void, ShaderSource, (GLuint shader, GLsizei count, const GLchar* const* strings,       \
                           const GLint* lengths))                                            \
    X(void, CompileShader, (GLuint shader))                                                  \
    X(void, GetShaderiv, (GLuint shader, GLenum pname, GLint * value))                       \
    X(void, GetShaderInfoLog, (GLuint shader, GLsizei size, GLsizei * len, GLchar * log))    \
    X(void, DeleteShader, (GLuint shader))                                                   \
    X(GLuint, CreateProgram, (void))                                                         \
    X(void, AttachShader, (GLuint program, GLuint shader))                                   \
    X(void, LinkProgram, (GLuint program))                                                   \
    X(void, GetProgramiv, (GLuint program, GLenum pname, GLint * value))                     \
    X(void, GetProgramInfoLog, (GLuint program, GLsizei size, GLsizei * len, GLchar * log))  \
    X(void, DeleteProgram, (GLuint program))                                                 \
    X(void, UseProgram, (GLuint program))                                                    \
    X(GLint, GetUniformLocation, (GLuint program, const GLchar* name))                       \
    X(void, Uniform1i, (GLint location, GLint v0))                                           \
    X(void, Uniform1f, (GLint location, GLfloat v0))                                         \
    X(void, Uniform2f, (GLint location, GLfloat v0, GLfloat v1))                             \
    X(void, Uniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))     \
    X(void, GenVertexArrays, (GLsizei n, GLuint * arrays))                                   \
    X(void, DeleteVertexArrays, (GLsizei n, const GLuint* arrays))                           \
    X(void, BindVertexArray, (GLuint array))                                                 \
    X(void, BindBuffer, (GLenum target, GLuint buffer))                                      \
    X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count))

// May be missing (GL 3.2 has no samplers; GLES has neither glPolygonMode nor
// glBindFragDataLocation).
#define RRM_GL_OPTIONAL(X)                                                      \
    X(void, BindSampler, (GLuint unit, GLuint sampler))                         \
    X(void, PolygonMode, (GLenum face, GLenum mode))                            \
    X(void, BindFragDataLocation, (GLuint program, GLuint color, const GLchar* name))

namespace rrm::lr {

struct GlFunctions {
#define RRM_GL_MEMBER(ret, name, args) ret(RRM_GLAPIENTRY* name) args = nullptr;
    RRM_GL_REQUIRED(RRM_GL_MEMBER)
    RRM_GL_OPTIONAL(RRM_GL_MEMBER)
#undef RRM_GL_MEMBER

    // Needs a current context. Returns false and names the first missing entry point.
    bool load(std::string& missing);
};

// Links the shared full-viewport quad vertex shader (uniform vec4 uTexRect maps the quad's
// bottom-left/top-right corners to texture coordinates, output "vUv") with a fragment shader
// body that writes "fragColor". The GLSL version header is added for desktop GL or GLES.
// Returns 0 with `error` set on failure.
GLuint buildQuadProgram(GlFunctions& gl, bool gles, const char* fragmentBody, std::string& error);

}  // namespace rrm::lr
