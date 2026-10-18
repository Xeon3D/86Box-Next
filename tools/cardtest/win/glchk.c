/* glchk -- OpenGL ICD conformance. Each case is rendered by the card's
   ICD into the window's back buffer and by Microsoft's generic software
   OpenGL into a 32 bpp DIB (the oracle); both are read back with
   glReadPixels and compared with a tolerance, as in D3DCHK.

   Also logs the ICD's strings, extensions and every pixel format.
   opengl32.dll is loaded at run time, so on Windows 95 without it the
   program reports a skip. Switches: /Q no pauses. */
#include "ct.h"
#include <GL/gl.h>

#define TW 256
#define TH 256

/* opengl32 entry points, looked up at run time. */
#define GLFUNCS                                                                                    \
    X(void, glBegin, (GLenum))                                                                     \
    X(void, glEnd, (void))                                                                         \
    X(void, glVertex3f, (GLfloat, GLfloat, GLfloat))                                               \
    X(void, glColor4ub, (GLubyte, GLubyte, GLubyte, GLubyte))                                      \
    X(void, glTexCoord2f, (GLfloat, GLfloat))                                                      \
    X(void, glEnable, (GLenum))                                                                    \
    X(void, glDisable, (GLenum))                                                                   \
    X(void, glClearColor, (GLclampf, GLclampf, GLclampf, GLclampf))                                \
    X(void, glClear, (GLbitfield))                                                                 \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei))                                          \
    X(void, glMatrixMode, (GLenum))                                                                \
    X(void, glLoadIdentity, (void))                                                                \
    X(void, glOrtho, (GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble))                 \
    X(void, glFrustum, (GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble))               \
    X(void, glTranslatef, (GLfloat, GLfloat, GLfloat))                                             \
    X(void, glRotatef, (GLfloat, GLfloat, GLfloat, GLfloat))                                       \
    X(void, glDepthFunc, (GLenum))                                                                 \
    X(void, glBlendFunc, (GLenum, GLenum))                                                         \
    X(void, glAlphaFunc, (GLenum, GLclampf))                                                       \
    X(void, glShadeModel, (GLenum))                                                                \
    X(void, glFogi, (GLenum, GLint))                                                               \
    X(void, glFogf, (GLenum, GLfloat))                                                             \
    X(void, glFogfv, (GLenum, const GLfloat *))                                                    \
    X(void, glGenTextures, (GLsizei, GLuint *))                                                    \
    X(void, glBindTexture, (GLenum, GLuint))                                                       \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint))                                              \
    X(void, glTexEnvi, (GLenum, GLenum, GLint))                                                    \
    X(void, glTexEnvfv, (GLenum, GLenum, const GLfloat *))                                         \
    X(void, glPixelStorei, (GLenum, GLint))                                                        \
    X(void, glReadBuffer, (GLenum))                                                                \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *))              \
    X(void, glDrawPixels, (GLsizei, GLsizei, GLenum, GLenum, const GLvoid *))                      \
    X(void, glCopyPixels, (GLint, GLint, GLsizei, GLsizei, GLenum))                                \
    X(void, glRasterPos2i, (GLint, GLint))                                                         \
    X(void, glPixelZoom, (GLfloat, GLfloat))                                                       \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei))                                           \
    X(void, glStencilFunc, (GLenum, GLint, GLuint))                                                \
    X(void, glStencilOp, (GLenum, GLenum, GLenum))                                                 \
    X(void, glClearStencil, (GLint))                                                               \
    X(void, glLineWidth, (GLfloat))                                                                \
    X(void, glPointSize, (GLfloat))                                                                \
    X(void, glLogicOp, (GLenum))                                                                   \
    X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean))                             \
    X(void, glFinish, (void))                                                                      \
    X(void, glGetIntegerv, (GLenum, GLint *))                                                      \
    X(const GLubyte *, glGetString, (GLenum))                                                      \
    X(HGLRC, wglCreateContext, (HDC))                                                              \
    X(BOOL, wglMakeCurrent, (HDC, HGLRC))                                                          \
    X(BOOL, wglDeleteContext, (HGLRC))

#define X(ret, name, args) static ret(WINAPI *p_##name) args;
GLFUNCS
#undef X

static HDC     wdc, mdc;
static HGLRC   rc_icd, rc_ref;
static GLuint  tex[2][4];
static DWORD  *buf_r;
static BYTE   *raw;
static int     quick, cur, stencil_ok;

static int
load_gl(void)
{
    HMODULE m = LoadLibraryA("OPENGL32.DLL");

    if (!m)
        return 0;
#define X(ret, name, args)                                         \
    p_##name = (ret(WINAPI *) args) GetProcAddress(m, #name);      \
    if (!p_##name)                                                 \
        return 0;
    GLFUNCS
#undef X
    return 1;
}

static DWORD
texel(int kind, int level, int x, int y)
{
    static const DWORD lv[8] = { 0xff0000ff, 0xff00ff00, 0xffff0000, 0xff00ffff, 0xffff00ff, 0xffffff00, 0xffffffff, 0xff808080 };

    switch (kind) {
        case 0: /* checker + gradient (ABGR as bytes R,G,B,A) */
            return 0xff000000 | (((x ^ y) & 8) ? 0xe0e0e0 : 0x202020) | ((y * 4) << 16 & 0xff0000) | ((x * 4) & 0xff);
        case 1: /* alpha holes */
            return ((((x >> 3) + (y >> 3)) & 1) ? 0xff000000 : 0x40000000) | ((x * 4) << 8 & 0xff00) | 0xc0;
        case 2: /* mip levels */
            return ((x & 7) == 0 || (y & 7) == 0) ? 0xff000000 : lv[level & 7];
        default:
            return (y & 4) ? 0xffffffff : 0xffff6040;
    }
}

static void
make_textures(int side)
{
    static DWORD img[64 * 64];
    int          k, lvl, x, y, sz;

    p_glGenTextures(4, tex[side]);
    for (k = 0; k < 4; k++) {
        p_glBindTexture(GL_TEXTURE_2D, tex[side][k]);
        for (lvl = 0, sz = 64; sz >= 1; lvl++, sz >>= 1) {
            for (y = 0; y < sz; y++)
                for (x = 0; x < sz; x++)
                    img[y * sz + x] = texel(k, lvl, x * 64 / sz, y * 64 / sz);
            p_glTexImage2D(GL_TEXTURE_2D, lvl, GL_RGBA, sz, sz, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
            if (k != 2)
                break;
        }
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
}

static void
reset_state(void)
{
    p_glViewport(0, 0, TW, TH);
    p_glMatrixMode(GL_PROJECTION);
    p_glLoadIdentity();
    p_glOrtho(0, TW, TH, 0, -1, 1);
    p_glMatrixMode(GL_MODELVIEW);
    p_glLoadIdentity();
    p_glDisable(GL_DITHER);
    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_BLEND);
    p_glDisable(GL_ALPHA_TEST);
    p_glDisable(GL_FOG);
    p_glDisable(GL_TEXTURE_2D);
    p_glDisable(GL_SCISSOR_TEST);
    p_glDisable(GL_STENCIL_TEST);
    p_glDisable(GL_LOGIC_OP);
    p_glDisable(GL_CULL_FACE);
    p_glShadeModel(GL_SMOOTH);
    p_glDepthFunc(GL_LESS);
    p_glColorMask(1, 1, 1, 1);
    p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    p_glClearColor(0, 0, 0, 1);
    p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | (stencil_ok ? GL_STENCIL_BUFFER_BIT : 0));
}

static void
quad(float x0, float y0, float x1, float y1, float z, DWORD c, float u0, float v0, float u1, float v1)
{
    p_glColor4ub((GLubyte) c, (GLubyte) (c >> 8), (GLubyte) (c >> 16), (GLubyte) (c >> 24));
    p_glBegin(GL_QUADS);
    p_glTexCoord2f(u0, v0);
    p_glVertex3f(x0, y0, z);
    p_glTexCoord2f(u1, v0);
    p_glVertex3f(x1, y0, z);
    p_glTexCoord2f(u1, v1);
    p_glVertex3f(x1, y1, z);
    p_glTexCoord2f(u0, v1);
    p_glVertex3f(x0, y1, z);
    p_glEnd();
}

static void
use_tex(int k, GLint filt)
{
    p_glEnable(GL_TEXTURE_2D);
    p_glBindTexture(GL_TEXTURE_2D, tex[cur][k]);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt == GL_NEAREST ? GL_NEAREST : GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

/* A floor in a real perspective projection. */
static void
floor_persp(float rep)
{
    p_glMatrixMode(GL_PROJECTION);
    p_glLoadIdentity();
    p_glFrustum(-1, 1, -1, 1, 1, 50);
    p_glMatrixMode(GL_MODELVIEW);
    p_glLoadIdentity();
    p_glColor4ub(255, 255, 255, 255);
    p_glBegin(GL_QUADS);
    p_glTexCoord2f(0, 0);
    p_glVertex3f(-4, -1, -1.2f);
    p_glTexCoord2f(rep, 0);
    p_glVertex3f(4, -1, -1.2f);
    p_glTexCoord2f(rep, rep);
    p_glVertex3f(4, -1, -40);
    p_glTexCoord2f(0, rep);
    p_glVertex3f(-4, -1, -40);
    p_glEnd();
}

static int
g_flat(const char **why)
{
    int i;

    for (i = 0; i < 2; i++) {
        p_glShadeModel(i ? GL_SMOOTH : GL_FLAT);
        p_glBegin(GL_TRIANGLES);
        p_glColor4ub(255, 0, 0, 255);
        p_glVertex3f(10 + i * 120, 10, 0);
        p_glColor4ub(0, 255, 0, 255);
        p_glVertex3f(120 + i * 120, 60, 0);
        p_glColor4ub(0, 0, 255, 255);
        p_glVertex3f(40 + i * 120, 240, 0);
        p_glEnd();
    }
    return 1;
}

static int
g_depth(const char **why)
{
    int f;

    p_glEnable(GL_DEPTH_TEST);
    p_glDepthFunc(GL_ALWAYS);
    p_glBegin(GL_QUADS); /* z ramps -1..1 left to right */
    p_glColor4ub(64, 64, 64, 255);
    p_glVertex3f(0, 0, 1);
    p_glVertex3f(256, 0, -1);
    p_glVertex3f(256, 256, -1);
    p_glVertex3f(0, 256, 1);
    p_glEnd();
    /* GL_LESS .. GL_ALWAYS; GL_NEVER is left out because Microsoft's
       software renderer (the reference) draws with it. */
    for (f = 1; f < 8; f++) {
        p_glDepthFunc(GL_NEVER + f);
        quad(0, (f - 1) * 36 + 2, 256, (f - 1) * 36 + 34, 0, 0xff000000 | ((f + 1) * 0x1f2f3f), 0, 0, 0, 0);
    }
    return 1;
}

/* 4x4 RGB texture with known texels, each drawn as a 64x64 block:
   checks the upload path on its own. */
static int
g_texsimple(const char **why)
{
    static const GLubyte t[4 * 4 * 3] = {
        255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255, 255, 0, 0, 255, 255, 255, 0, 255, 0, 0, 0,
        128, 0, 0, 0, 128, 0, 0, 0, 128, 128, 128, 128, 64, 32, 16, 16, 32, 64, 200, 100, 50, 50, 100, 200
    };
    GLuint id;

    p_glGenTextures(1, &id);
    p_glBindTexture(GL_TEXTURE_2D, id);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4, 4, 0, GL_RGB, GL_UNSIGNED_BYTE, t);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glEnable(GL_TEXTURE_2D);
    p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    quad(0, 0, 256, 256, 0, 0xffffffff, 0, 0, 1, 1);
    return 1;
}

static int
g_texnearest(const char **why)
{
    use_tex(0, GL_NEAREST);
    quad(0, 0, 128, 128, 0, 0xffffffff, 0, 0, 1, 1);
    quad(128, 0, 160, 32, 0, 0xffffffff, 0, 0, 1, 1);
    quad(0, 128, 256, 256, 0, 0xff80ffff, 0.3f, 0.2f, 2.3f, 1.2f);
    return 1;
}

static int
g_texlinear(const char **why)
{
    use_tex(0, GL_LINEAR);
    quad(0, 0, 256, 256, 0, 0xffffffff, 0, 0, 0.5f, 0.5f);
    return 1;
}

static int
g_persp(const char **why)
{
    use_tex(0, GL_NEAREST);
    floor_persp(8);
    return 1;
}

static int
g_blend(const char **why)
{
    int i;

    quad(0, 0, 256, 128, 0, 0xffff5020, 0, 0, 0, 0);
    quad(0, 128, 256, 256, 0, 0xff20e0ff, 0, 0, 0, 0);
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (i = 0; i < 8; i++)
        quad(10 + i * 30, 20, 40 + i * 30, 236, 0, ((DWORD) (i * 36) << 24) | 0x2020ff, 0, 0, 0, 0);
    p_glBlendFunc(GL_ONE, GL_ONE);
    quad(60, 90, 200, 170, 0, 0xff404040, 0, 0, 0, 0);
    p_glBlendFunc(GL_DST_COLOR, GL_ZERO);
    quad(30, 190, 220, 230, 0, 0xff80ff80, 0, 0, 0, 0);
    return 1;
}

static int
g_alphatest(const char **why)
{
    quad(0, 0, 256, 256, 0, 0xffa04020, 0, 0, 0, 0);
    use_tex(1, GL_NEAREST);
    p_glEnable(GL_ALPHA_TEST);
    p_glAlphaFunc(GL_GREATER, 0.5f);
    quad(0, 0, 256, 256, 0, 0xffffffff, 0, 0, 2, 2);
    return 1;
}

static int
g_fog(const char **why)
{
    static const GLfloat fc[4] = { 0.75f, 0.7f, 0.6f, 1 };

    use_tex(0, GL_NEAREST);
    p_glEnable(GL_FOG);
    p_glFogi(GL_FOG_MODE, GL_LINEAR);
    p_glFogf(GL_FOG_START, 2);
    p_glFogf(GL_FOG_END, 30);
    p_glFogfv(GL_FOG_COLOR, fc);
    floor_persp(8);
    return 1;
}

static int
g_mipmap(const char **why)
{
    use_tex(2, GL_NEAREST_MIPMAP_NEAREST);
    floor_persp(16);
    return 1;
}

static int
g_trilinear(const char **why)
{
    use_tex(2, GL_LINEAR_MIPMAP_LINEAR);
    floor_persp(16);
    return 1;
}

static int
g_texwrap(const char **why)
{
    use_tex(0, GL_NEAREST);
    quad(0, 0, 127, 256, 0, 0xffffffff, -1, -1, 2, 2);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    quad(129, 0, 256, 256, 0, 0xffffffff, -1, -1, 2, 2);
    return 1;
}

static int
g_texenv(const char **why)
{
    static const GLint   modes[4] = { GL_MODULATE, GL_DECAL, GL_REPLACE, GL_BLEND };
    static const GLfloat ec[4]    = { 1, 0.5f, 0, 1 };
    int                  i;

    use_tex(1, GL_NEAREST);
    p_glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, ec);
    for (i = 0; i < 4; i++) {
        p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, modes[i]);
        quad((i & 1) * 128, (i >> 1) * 128, (i & 1) * 128 + 128, (i >> 1) * 128 + 128, 0, 0xffc08040, 0, 0, 1, 1);
    }
    return 1;
}

static int
g_lines(const char **why)
{
    int i;

    p_glBegin(GL_LINES);
    for (i = 0; i < 48; i++) {
        p_glColor4ub((GLubyte) (i * 40), (GLubyte) (255 - i * 5), (GLubyte) (i * 17), 255);
        p_glVertex3f(128.5f, 128.5f, 0);
        p_glVertex3f(128.5f + (i < 12 ? 120 : i < 24 ? 120 - (i - 12) * 20 : i < 36 ? -120 : -120 + (i - 36) * 20),
                     128.5f + (i < 12 ? -120 + i * 20 : i < 24 ? 120 : i < 36 ? 120 - (i - 24) * 20 : -120), 0);
    }
    p_glEnd();
    p_glPointSize(1);
    p_glBegin(GL_POINTS);
    for (i = 0; i < 200; i++) {
        p_glColor4ub(255, 255, (GLubyte) i, 255);
        p_glVertex3f((float) ((i * 37) % 256) + 0.5f, (float) ((i * 91) % 256) + 0.5f, 0);
    }
    p_glEnd();
    return 1;
}

static int
g_stencil(const char **why)
{
    if (!stencil_ok) {
        *why = "no stencil buffer in one of the pixel formats";
        return 0;
    }
    p_glEnable(GL_STENCIL_TEST);
    p_glStencilFunc(GL_ALWAYS, 1, 0xff);
    p_glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    p_glColorMask(0, 0, 0, 0);
    quad(40, 40, 216, 216, 0, 0xffffffff, 0, 0, 0, 0);
    p_glColorMask(1, 1, 1, 1);
    p_glStencilFunc(GL_EQUAL, 1, 0xff);
    p_glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    use_tex(0, GL_NEAREST);
    quad(0, 0, 256, 256, 0, 0xffffffff, 0, 0, 2, 2);
    return 1;
}

static int
g_pixels(const char **why)
{
    static DWORD img[64 * 64];
    int          x, y;

    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            img[y * 64 + x] = texel(0, 0, x, y);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    p_glRasterPos2i(10, 74);
    p_glPixelZoom(1, 1);
    p_glDrawPixels(64, 64, GL_RGBA, GL_UNSIGNED_BYTE, img);
    p_glRasterPos2i(100, 250);
    p_glPixelZoom(2, 2);
    p_glDrawPixels(64, 64, GL_RGBA, GL_UNSIGNED_BYTE, img);
    p_glPixelZoom(1, 1);
    /* Copy within the frame buffer (window coordinates, y up). */
    p_glRasterPos2i(10, 200);
    p_glCopyPixels(10, TH - 74, 64, 64, GL_COLOR);
    p_glEnable(GL_SCISSOR_TEST);
    p_glScissor(180, 20, 60, 100);
    p_glClearColor(0.2f, 0.6f, 0.3f, 1);
    p_glClear(GL_COLOR_BUFFER_BIT);
    return 1;
}

static int
g_logicop(const char **why)
{
    quad(0, 0, 256, 256, 0, 0xff3060c0, 0, 0, 0, 0);
    p_glEnable(GL_COLOR_LOGIC_OP);
    p_glLogicOp(GL_XOR);
    quad(40, 40, 216, 216, 0, 0xffffffff, 0, 0, 0, 0);
    p_glLogicOp(GL_INVERT);
    quad(80, 0, 120, 256, 0, 0xffffffff, 0, 0, 0, 0);
    p_glDisable(GL_COLOR_LOGIC_OP);
    return 1;
}

typedef int (*glcase)(const char **why);

/* tol = per-channel tolerance, radius = position slack in pixels, warn/
   fail = per-mille of bad pixels. */
static const struct {
    const char *name;
    glcase      fn;
    int         tol, radius, warn, fail;
} cases[] = {
    /* untextured first: a broken texture upload must not spoil them */
    { "FLATGOUR", g_flat, 24, 1, 5, 30 },       { "DEPTH", g_depth, 24, 1, 5, 30 },
    { "BLEND", g_blend, 24, 1, 5, 30 },         { "LINES", g_lines, 24, 1, 10, 50 },
    { "PIXELS", g_pixels, 24, 1, 5, 30 },       { "LOGICOP", g_logicop, 24, 1, 5, 30 },
    { "TEXNEAR", g_texnearest, 24, 1, 5, 30 },  { "TEXLIN", g_texlinear, 32, 2, 5, 30 },
    { "PERSP", g_persp, 40, 2, 10, 50 },        { "ALPHATST", g_alphatest, 24, 1, 5, 30 },
    { "FOG", g_fog, 40, 2, 10, 50 },            { "MIPMAP", g_mipmap, 40, 2, 20, 100 },
    { "TRILIN", g_trilinear, 48, 2, 20, 100 },  { "TEXWRAP", g_texwrap, 24, 1, 5, 30 },
    { "TEXENV", g_texenv, 24, 1, 5, 30 },       { "STENCIL", g_stencil, 24, 1, 5, 30 },
    /* last: a 48-byte RGB upload with 1-byte row alignment */
    { "TEXSMPL", g_texsimple, 16, 1, 5, 30 },
};
#define NCASES ((int) (sizeof(cases) / sizeof(cases[0])))

static void
readback(DWORD *out)
{
    int x, y;

    p_glFinish();
    p_glPixelStorei(GL_PACK_ALIGNMENT, 4);
    p_glReadPixels(0, 0, TW, TH, GL_RGBA, GL_UNSIGNED_BYTE, raw);
    for (y = 0; y < TH; y++)
        for (x = 0; x < TW; x++) {
            BYTE *p                  = raw + ((TH - 1 - y) * TW + x) * 4;
            out[y * TW + x]          = (p[0] << 16) | (p[1] << 8) | p[2];
        }
}

static void
log_formats(void)
{
    PIXELFORMATDESCRIPTOR pfd;
    int                   n = DescribePixelFormat(wdc, 1, sizeof(pfd), &pfd), i;

    for (i = 1; i <= n; i++) {
        DescribePixelFormat(wdc, i, sizeof(pfd), &pfd);
        if (!(pfd.dwFlags & PFD_SUPPORT_OPENGL))
            continue;
        ct_log("INFO  pf %d flags=%08x %s color=%d alpha=%d depth=%d stencil=%d accum=%d", i, pfd.dwFlags,
               (pfd.dwFlags & PFD_GENERIC_FORMAT) ? ((pfd.dwFlags & PFD_GENERIC_ACCELERATED) ? "MCD" : "generic") : "ICD",
               pfd.cColorBits, pfd.cAlphaBits, pfd.cDepthBits, pfd.cStencilBits, pfd.cAccumBits);
    }
}

void __stdcall
entry(void)
{
    PIXELFORMATDESCRIPTOR pfd;
    HWND                  hw;
    HBITMAP               dib;
    BITMAPINFOHEADER      bi;
    void                 *bits;
    const char           *why;
    static DWORD         *store[32];
    static const char    *whys[32];
    static int            skipped[32];
    int                   pf, i, s1, s2;

    ct_open("GLCHK");
    quick = ct_flag("/Q");
    if (!load_gl()) {
        ct_skip("ALL", "opengl32.dll missing or incomplete");
        ct_close();
        ExitProcess(2);
    }
    hw  = ct_window(TW * 2 + 8, TH);
    wdc = GetDC(hw);
    log_formats();

    /* The card: double-buffered RGBA with depth and stencil. */
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 32;
    pfd.cDepthBits   = 16;
    pfd.cStencilBits = 8;
    pf               = ChoosePixelFormat(wdc, &pfd);
    SetPixelFormat(wdc, pf, &pfd);
    DescribePixelFormat(wdc, pf, sizeof(pfd), &pfd);
    s1 = pfd.cStencilBits;
    ct_log("INFO  window pixel format %d: color=%d depth=%d stencil=%d %s", pf, pfd.cColorBits, pfd.cDepthBits,
           pfd.cStencilBits, (pfd.dwFlags & PFD_GENERIC_FORMAT) ? "(software!)" : "(ICD)");
    rc_icd = p_wglCreateContext(wdc);
    if (!rc_icd || !p_wglMakeCurrent(wdc, rc_icd)) {
        ct_skip("ALL", "cannot create the window context");
        goto done;
    }
    ct_log("INFO  vendor   %s", (const char *) p_glGetString(GL_VENDOR));
    ct_log("INFO  renderer %s", (const char *) p_glGetString(GL_RENDERER));
    ct_log("INFO  version  %s", (const char *) p_glGetString(GL_VERSION));
    ct_log_long("ext", (const char *) p_glGetString(GL_EXTENSIONS));
    if ((pfd.dwFlags & PFD_GENERIC_FORMAT) && !(pfd.dwFlags & PFD_GENERIC_ACCELERATED)) {
        ct_skip("ALL", "no OpenGL ICD for this card/colour depth (only Microsoft's software renderer)");
        goto done;
    }
    cur = 0;
    make_textures(0);
    stencil_ok = s1 != 0;
    raw        = (BYTE *) GlobalAlloc(GPTR, TW * TH * 4);

    /* Phase 1: every case on the card, in its own context only. */
    for (i = 0; i < NCASES; i++) {
        why        = "";
        store[i]   = (DWORD *) GlobalAlloc(GPTR, TW * TH * 4);
        reset_state();
        skipped[i] = !cases[i].fn(&why);
        whys[i]    = why;
        if (skipped[i])
            continue;
        p_glReadBuffer(GL_BACK);
        readback(store[i]);
        SwapBuffers(wdc);
        ct_pump(quick ? 50 : 400);
    }
    p_wglMakeCurrent(NULL, NULL);

    /* Phase 2: the reference, Microsoft generic OpenGL on a 32 bpp DIB. */
    memset(&bi, 0, sizeof(bi));
    bi.biSize     = sizeof(bi);
    bi.biWidth    = TW;
    bi.biHeight   = TH;
    bi.biPlanes   = 1;
    bi.biBitCount = 32;
    dib           = CreateDIBSection(wdc, (BITMAPINFO *) &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    mdc           = CreateCompatibleDC(wdc);
    SelectObject(mdc, dib);
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_BITMAP | PFD_SUPPORT_OPENGL | PFD_SUPPORT_GDI;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 32;
    pfd.cDepthBits   = 16;
    pfd.cStencilBits = 8;
    pf               = ChoosePixelFormat(mdc, &pfd);
    SetPixelFormat(mdc, pf, &pfd);
    DescribePixelFormat(mdc, pf, sizeof(pfd), &pfd);
    s2     = pfd.cStencilBits;
    rc_ref = p_wglCreateContext(mdc);
    if (!rc_ref || !p_wglMakeCurrent(mdc, rc_ref)) {
        ct_skip("ALL", "cannot create the software reference context");
        goto done;
    }
    cur = 1;
    make_textures(1);
    buf_r = (DWORD *) GlobalAlloc(GPTR, TW * TH * 4);
    for (i = 0; i < NCASES; i++) {
        if (skipped[i]) {
            ct_skip(cases[i].name, whys[i]);
            continue;
        }
        if (cases[i].fn == g_stencil && !s2) {
            ct_skip(cases[i].name, "the reference has no stencil buffer");
            continue;
        }
        reset_state();
        cases[i].fn(&why);
        readback(buf_r);
        BitBlt(wdc, TW + 8, 0, TW, TH, mdc, 0, 0, SRCCOPY);
        ct_grade_r(cases[i].name, store[i], buf_r, TW, TH, cases[i].tol, cases[i].radius, cases[i].warn,
                   cases[i].fail, "");
        ct_pump(quick ? 50 : 400);
    }
done:
    p_wglMakeCurrent(NULL, NULL);
    ct_close();
    ExitProcess(ct_nfail ? 1 : 0);
}
