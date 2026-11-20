/* gl.c - compiles wallpaper shaders and draws them fullscreen (OpenGL ES 2.0).
 *
 * A shader is prelude.glsl + body + epilogue.glsl. Custom files can also be
 * Shadertoy-style (mainImage(out vec4, in vec2)), which gets a small adapter.
 * When the shader renders below full resolution, it draws into a smaller
 * texture that is then stretched over the surface. */
#include "lw.h"
#include "builtin_shaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Shader {
    GLuint prog;
    GLint u_res, u_phase, u_time, u_mouse, u_click, u_click_age, u_mouse_on;
};

static const char *kVS =
    "attribute vec2 p;\n"
    "varying vec2 uv;\n"
    "void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";

static const char *kShadertoy =
    "#define iResolution vec3(res, 1.0)\n"
    "#define iTime time\n"
    "#define iTimeDelta 0.0\n"
    "#define iFrame 0\n"
    "#define iMouse vec4(mouse.x, res.y - mouse.y, click.x, res.y - click.y)\n";

static const char *kShadertoyMain =
    "\nvec4 ps_main(vec2 pos)\n"
    "{\n"
    "    vec4 c = vec4(0.0);\n"
    "    mainImage(c, vec2(pos.x, res.y - pos.y));\n"
    "    return vec4(clamp(c.rgb, 0.0, 1.0), 1.0);\n"
    "}\n";

static GLuint quad_vbo;

static GLuint compile(GLenum type, const char *const *parts, int n)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, n, parts, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        lw_log("shader compile error:\n%s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link(const char *const *fs_parts, int n)
{
    GLuint vs = compile(GL_VERTEX_SHADER, &kVS, 1);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_parts, n);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "p");
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        lw_log("shader link error:\n%s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static void quad(void)
{
    if (!quad_vbo) {
        static const float tri[] = {-1, -1, 3, -1, -1, 3}; /* one triangle covers the screen */
        glGenBuffers(1, &quad_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
    }
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (1 << 20)) { fclose(f); return NULL; }
    char *s = malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

Shader *shader_load(const char *id)
{
    const Builtin *b = builtin_find(id);
    char *custom = NULL;
    const char *parts[5];
    int n = 0;
    parts[n++] = SRC_PRELUDE;
    if (b) {
        parts[n++] = b->src;
    } else {
        custom = read_file(id);
        if (!custom) { lw_log("can't read shader %s", id); return NULL; }
        if (strstr(custom, "mainImage")) {
            parts[n++] = kShadertoy;
            parts[n++] = custom;
            parts[n++] = kShadertoyMain;
        } else {
            parts[n++] = custom;
        }
    }
    parts[n++] = SRC_EPILOGUE;
    GLuint p = link(parts, n);
    free(custom);
    if (!p) return NULL;
    Shader *s = calloc(1, sizeof *s);
    s->prog = p;
    s->u_res = glGetUniformLocation(p, "res");
    s->u_phase = glGetUniformLocation(p, "phase");
    s->u_time = glGetUniformLocation(p, "time");
    s->u_mouse = glGetUniformLocation(p, "mouse");
    s->u_click = glGetUniformLocation(p, "click");
    s->u_click_age = glGetUniformLocation(p, "click_age");
    s->u_mouse_on = glGetUniformLocation(p, "mouse_on");
    return s;
}

/* Lower-resolution rendering: a texture the shader draws into, then a
   stretch onto the surface. One per context is enough (sizes are rebuilt). */
static GLuint low_fbo, low_tex, blit_prog;
static int low_w, low_h;

static void ensure_low(int w, int h)
{
    if (low_fbo && low_w == w && low_h == h) return;
    if (!low_fbo) { glGenFramebuffers(1, &low_fbo); glGenTextures(1, &low_tex); }
    glBindTexture(GL_TEXTURE_2D, low_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, low_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, low_tex, 0);
    low_w = w;
    low_h = h;
    if (!blit_prog) {
        static const char *fs =
            "precision mediump float;\n"
            "varying vec2 uv;\n"
            "uniform sampler2D tex;\n"
            "void main() { gl_FragColor = texture2D(tex, uv); }\n";
        blit_prog = link(&fs, 1);
    }
}

void shader_draw(Shader *s, int w, int h, const Frame *f, int scale)
{
    GLint target = 0;
    int rw = w, rh = h;
    if (!s) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    if (scale > 1) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
        rw = (w + scale - 1) / scale;
        rh = (h + scale - 1) / scale;
        ensure_low(rw, rh);
        glBindFramebuffer(GL_FRAMEBUFFER, low_fbo);
    }
    glViewport(0, 0, rw, rh);
    glDisable(GL_BLEND);
    glUseProgram(s->prog);
    float k = (float)rw / (float)w;
    glUniform2f(s->u_res, (float)rw, (float)rh);
    glUniform1f(s->u_phase, f->phase);
    glUniform1f(s->u_time, (float)f->time);
    glUniform2f(s->u_mouse, f->mouse[0] * k, f->mouse[1] * k);
    glUniform2f(s->u_click, f->click[0] * k, f->click[1] * k);
    glUniform1f(s->u_click_age, f->click_age);
    glUniform1f(s->u_mouse_on, f->mouse_on);
    quad();
    if (scale > 1) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)target);
        glViewport(0, 0, w, h);
        glUseProgram(blit_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, low_tex);
        glUniform1i(glGetUniformLocation(blit_prog, "tex"), 0);
        quad();
    }
}

void shader_free(Shader *s)
{
    if (!s) return;
    glDeleteProgram(s->prog);
    free(s);
}

void dim_draw(float brightness)
{
    static GLuint prog;
    static GLint u_a;
    if (brightness >= 0.999f) return;
    if (!prog) {
        static const char *fs =
            "precision mediump float;\n"
            "uniform float a;\n"
            "void main() { gl_FragColor = vec4(0.0, 0.0, 0.0, a); }\n";
        prog = link(&fs, 1);
        u_a = glGetUniformLocation(prog, "a");
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog);
    glUniform1f(u_a, 1.0f - brightness);
    quad();
    glDisable(GL_BLEND);
}
