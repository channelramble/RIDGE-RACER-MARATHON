#include "libretro/GlFunctions.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace rrm::lr {

bool GlFunctions::load(std::string& missing) {
    missing.clear();
#define RRM_GL_LOAD(ret, name, args)                                                     \
    name = reinterpret_cast<ret(RRM_GLAPIENTRY*) args>(SDL_GL_GetProcAddress("gl" #name)); \
    if (!name && missing.empty()) missing = "gl" #name;
    RRM_GL_REQUIRED(RRM_GL_LOAD)
#undef RRM_GL_LOAD
#define RRM_GL_LOAD_OPT(ret, name, args) \
    name = reinterpret_cast<ret(RRM_GLAPIENTRY*) args>(SDL_GL_GetProcAddress("gl" #name));
    RRM_GL_OPTIONAL(RRM_GL_LOAD_OPT)
#undef RRM_GL_LOAD_OPT
    return missing.empty();
}

namespace {

// Full-viewport quad from gl_VertexID (no vertex buffers). uTexRect maps the quad's
// bottom-left (xy) and top-right (zw) corners to texture coordinates, which also
// encodes the vertical flip for top-down images.
const char* kVertexBody = R"(
uniform vec4 uTexRect;
out vec2 vUv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    vUv = mix(uTexRect.xy, uTexRect.zw, p);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

GLuint compile(GlFunctions& gl, GLenum type, const std::string& src, std::string& error) {
    GLuint shader = gl.CreateShader(type);
    const GLchar* text = src.c_str();
    gl.ShaderSource(shader, 1, &text, nullptr);
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        gl.GetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(std::max(len, 1)), '\0');
        gl.GetShaderInfoLog(shader, len, nullptr, log.data());
        error = "shader compile failed: " + log;
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

GLuint buildQuadProgram(GlFunctions& g, bool gles, const char* fragmentBody, std::string& error) {
    const std::string header = gles ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                                    : "#version 150\n";
    const std::string fragOut = gles ? "layout(location = 0) out vec4 fragColor;\n" : "out vec4 fragColor;\n";
    GLuint vs = compile(g, GL_VERTEX_SHADER, header + kVertexBody, error);
    if (!vs) return 0;
    GLuint fs = compile(g, GL_FRAGMENT_SHADER, header + fragOut + fragmentBody, error);
    if (!fs) {
        g.DeleteShader(vs);
        return 0;
    }
    GLuint program = g.CreateProgram();
    g.AttachShader(program, vs);
    g.AttachShader(program, fs);
    if (!gles && g.BindFragDataLocation) g.BindFragDataLocation(program, 0, "fragColor");
    g.LinkProgram(program);
    g.DeleteShader(vs);  // flagged; freed with the program
    g.DeleteShader(fs);
    GLint ok = 0;
    g.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        g.GetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(std::max(len, 1)), '\0');
        g.GetProgramInfoLog(program, len, nullptr, log.data());
        error = "shader link failed: " + log;
        g.DeleteProgram(program);
        return 0;
    }
    return program;
}

}  // namespace rrm::lr
