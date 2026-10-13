/* GLideN64 on ANGLE: see angle_ctx.h.
 *
 * ANGLE is loaded at runtime from <core folder>/angle/ (libGLESv2.dylib,
 * libEGL.dylib) so the core still loads and runs on the system OpenGL when
 * the libraries are missing. Everything here talks to ANGLE through function
 * pointers fetched from those libraries; nothing links against EGL or GLES. */

#include "angle_ctx.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* --- the few EGL / GLES definitions used here (Khronos values) --- */
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint;
typedef unsigned EGLBoolean, EGLenum;
typedef intptr_t EGLAttrib;

#define EGL_NONE                         0x3038
#define EGL_ALPHA_SIZE                   0x3021
#define EGL_BLUE_SIZE                    0x3022
#define EGL_GREEN_SIZE                   0x3023
#define EGL_RED_SIZE                     0x3024
#define EGL_DEPTH_SIZE                   0x3025
#define EGL_STENCIL_SIZE                 0x3026
#define EGL_SURFACE_TYPE                 0x3033
#define EGL_RENDERABLE_TYPE              0x3040
#define EGL_PBUFFER_BIT                  0x0001
#define EGL_OPENGL_ES3_BIT               0x0040
#define EGL_HEIGHT                       0x3056
#define EGL_WIDTH                        0x3057
#define EGL_OPENGL_ES_API                0x30A0
#define EGL_CONTEXT_MAJOR_VERSION        0x3098
#define EGL_CONTEXT_MINOR_VERSION        0x30FB
#define EGL_PLATFORM_ANGLE_ANGLE         0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE    0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE 0x3453
#define EGL_NO_CONTEXT                   ((EGLContext)0)
#define EGL_NO_SURFACE                   ((EGLSurface)0)
#define EGL_NO_DISPLAY                   ((EGLDisplay)0)

typedef unsigned GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLubyte;

#define GL_UNSIGNED_BYTE                 0x1401
#define GL_RGBA                          0x1908
#define GL_RGBA8                         0x8058
#define GL_BGRA_EXT                      0x80E1
#define GL_DEPTH24_STENCIL8              0x88F0
#define GL_PACK_ALIGNMENT                0x0D05
#define GL_PIXEL_PACK_BUFFER             0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING     0x88ED
#define GL_READ_FRAMEBUFFER              0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING      0x8CAA
#define GL_FRAMEBUFFER                   0x8D40
#define GL_RENDERBUFFER                  0x8D41
#define GL_COLOR_ATTACHMENT0             0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT      0x821A
#define GL_FRAMEBUFFER_COMPLETE          0x8CD5
#define GL_FRAMEBUFFER_BINDING           0x8CA6
#define GL_RENDERBUFFER_BINDING          0x8CA7
#define GL_VERSION                       0x1F02
#define GL_RENDERER                      0x1F01
#define GL_EXTENSIONS                    0x1F03
#define GL_NUM_EXTENSIONS                0x821D

static struct
{
   void *gles, *egl;

   EGLDisplay (*GetPlatformDisplay)(EGLenum, void *, const EGLAttrib *);
   EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
   EGLBoolean (*Terminate)(EGLDisplay);
   EGLBoolean (*BindAPI)(EGLenum);
   EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
   EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
   EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
   EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
   EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
   EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
   retro_proc_address_t (*GetProcAddress)(const char *);
   EGLint (*GetError)(void);

   void (*glGenFramebuffers)(GLsizei, GLuint *);
   void (*glDeleteFramebuffers)(GLsizei, const GLuint *);
   void (*glBindFramebuffer)(GLenum, GLuint);
   void (*glGenRenderbuffers)(GLsizei, GLuint *);
   void (*glDeleteRenderbuffers)(GLsizei, const GLuint *);
   void (*glBindRenderbuffer)(GLenum, GLuint);
   void (*glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
   void (*glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
   GLenum (*glCheckFramebufferStatus)(GLenum);
   void (*glGetIntegerv)(GLenum, GLint *);
   void (*glPixelStorei)(GLenum, GLint);
   void (*glBindBuffer)(GLenum, GLuint);
   void (*glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
   const GLubyte *(*glGetString)(GLenum);
   const GLubyte *(*glGetStringi)(GLenum, GLuint);

   EGLDisplay display;
   EGLSurface surface;
   EGLContext context;
   GLuint fbo, color, depth;
   bool bgra;          /* GL_EXT_read_format_bgra: read straight into XRGB8888 */
   bool active;

   uint8_t *raw, *frame;
   size_t raw_size, frame_size;
} a;

static retro_log_printf_t angle_log;

#define ALOG(level, ...) do { if (angle_log) angle_log(level, "[ANGLE] " __VA_ARGS__); } while (0)

/* <folder of this core>/angle/<name> */
static void *load_lib(const char *name)
{
   Dl_info info;
   char path[4096];
   char *slash;
   void *h;

   if (!dladdr((const void *)&angle_ctx_create, &info) || !info.dli_fname)
      return NULL;
   snprintf(path, sizeof(path), "%s", info.dli_fname);
   slash = strrchr(path, '/');
   if (!slash)
      return NULL;
   snprintf(slash + 1, sizeof(path) - (slash + 1 - path), "angle/%s", name);

   h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (!h)
      ALOG(RETRO_LOG_ERROR, "can't load %s: %s\n", path, dlerror());
   return h;
}

static retro_proc_address_t angle_proc(const char *sym)
{
   retro_proc_address_t p = NULL;
   if (a.GetProcAddress)
      p = a.GetProcAddress(sym);
   if (!p && a.gles)
      p = (retro_proc_address_t)dlsym(a.gles, sym);
   return p;
}

/* Desktop-only entry points the core's GL state layer (glsm) calls, mapped
 * onto their OpenGL ES 3.0 equivalents. GLideN64 itself picks ES paths at
 * runtime; glsm is compiled for desktop OpenGL and still asks for these. */
#define GL_BUFFER_SIZE        0x8764
#define GL_READ_ONLY          0x88B8
#define GL_WRITE_ONLY         0x88B9
#define GL_MAP_READ_BIT       0x0001
#define GL_MAP_WRITE_BIT      0x0002

static void shim_glClearDepth(double d)
{
   static void (*f)(float);
   if (!f) *(void **)&f = (void *)angle_proc("glClearDepthf");
   f((float)d);
}

static void shim_glDepthRange(double n, double far_)
{
   static void (*f)(float, float);
   if (!f) *(void **)&f = (void *)angle_proc("glDepthRangef");
   f((float)n, (float)far_);
}

static void shim_glPolygonMode(GLenum face, GLenum mode)
{
   (void)face; (void)mode; /* fill only on ES */
}

static void *shim_glMapBuffer(GLenum target, GLenum access)
{
   static void (*getp)(GLenum, GLenum, GLint *);
   static void *(*map)(GLenum, intptr_t, intptr_t, GLbitfield);
   GLint size = 0;
   if (!getp) *(void **)&getp = (void *)angle_proc("glGetBufferParameteriv");
   if (!map) *(void **)&map = (void *)angle_proc("glMapBufferRange");
   getp(target, GL_BUFFER_SIZE, &size);
   return map(target, 0, size, access == GL_READ_ONLY ? GL_MAP_READ_BIT
         : access == GL_WRITE_ONLY ? GL_MAP_WRITE_BIT : GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
}

static void shim_glGetBufferSubData(GLenum target, intptr_t offset, intptr_t size, void *data)
{
   static void *(*map)(GLenum, intptr_t, intptr_t, GLbitfield);
   static unsigned char (*unmap)(GLenum);
   void *src;
   if (!map) *(void **)&map = (void *)angle_proc("glMapBufferRange");
   if (!unmap) *(void **)&unmap = (void *)angle_proc("glUnmapBuffer");
   if ((src = map(target, offset, size, GL_MAP_READ_BIT)))
   {
      memcpy(data, src, (size_t)size);
      unmap(target);
   }
}

retro_proc_address_t angle_ctx_get_proc_address(const char *sym)
{
   static const struct { const char *name; retro_proc_address_t fn; } shims[] = {
      { "glClearDepth",       (retro_proc_address_t)shim_glClearDepth },
      { "glDepthRange",       (retro_proc_address_t)shim_glDepthRange },
      { "glPolygonMode",      (retro_proc_address_t)shim_glPolygonMode },
      { "glMapBuffer",        (retro_proc_address_t)shim_glMapBuffer },
      { "glGetBufferSubData", (retro_proc_address_t)shim_glGetBufferSubData },
   };
   /* ES 3.2 / desktop entry points that ES 3.0 only has as extensions:
    * the core names would exist but fail with "requires OpenGL ES 3.2" */
   static const struct { const char *name, *ext; } aliases[] = {
      { "glEnablei",                "glEnableiEXT" },
      { "glDisablei",               "glDisableiEXT" },
      { "glIsEnabledi",             "glIsEnablediEXT" },
      { "glBlendEquationi",         "glBlendEquationiEXT" },
      { "glBlendEquationSeparatei", "glBlendEquationSeparateiEXT" },
      { "glBlendFunci",             "glBlendFunciEXT" },
      { "glBlendFuncSeparatei",     "glBlendFuncSeparateiEXT" },
      { "glColorMaski",             "glColorMaskiEXT" },
      { "glDrawElementsBaseVertex",          "glDrawElementsBaseVertexEXT" },
      { "glDrawRangeElementsBaseVertex",     "glDrawRangeElementsBaseVertexEXT" },
      { "glDrawElementsInstancedBaseVertex", "glDrawElementsInstancedBaseVertexEXT" },
      { "glClipControl",            "glClipControlEXT" },
      { "glPolygonMode",            "glPolygonModeANGLE" },
      { "glBufferStorage",          "glBufferStorageEXT" },
      { "glCopyImageSubData",       "glCopyImageSubDataEXT" },
   };
   retro_proc_address_t p;
   size_t i;
   for (i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
      if (!strcmp(sym, aliases[i].name))
      {
         /* NULL when ANGLE lacks the extension: GLideN64 checks for it */
         if ((p = angle_proc(aliases[i].ext)))
            return p;
         break;
      }
   if (i < sizeof(aliases) / sizeof(aliases[0]) && strcmp(sym, "glPolygonMode"))
      return NULL;
   p = angle_proc(sym);
   if (p)
      return p;
   for (i = 0; i < sizeof(shims) / sizeof(shims[0]); i++)
      if (!strcmp(sym, shims[i].name))
         return shims[i].fn;
   return NULL;
}

#define EGLSYM(field, name) \
   if (!(*(void **)&a.field = dlsym(a.egl, name))) { ALOG(RETRO_LOG_ERROR, "missing %s\n", name); goto fail; }
#define GLSYM(field) \
   if (!(*(void **)&a.field = (void *)angle_ctx_get_proc_address(#field))) { ALOG(RETRO_LOG_ERROR, "missing %s\n", #field); goto fail; }

static bool has_extension(const char *ext)
{
   GLint n = 0, i;
   a.glGetIntegerv(GL_NUM_EXTENSIONS, &n);
   for (i = 0; i < n; i++)
   {
      const char *e = (const char *)a.glGetStringi(GL_EXTENSIONS, (GLuint)i);
      if (e && !strcmp(e, ext))
         return true;
   }
   return false;
}

/* N64_ANGLE_DEBUG=1: log each distinct GL error/warning ANGLE reports */
#define GL_DEBUG_OUTPUT_SYNCHRONOUS 0x8242
#define GL_DEBUG_OUTPUT             0x92E0
static void debug_cb(GLenum source, GLenum type, GLuint id, GLenum severity,
      GLsizei length, const char *msg, const void *user)
{
   static char seen[64][160];
   static unsigned n_seen;
   unsigned i;
   (void)source; (void)id; (void)length; (void)user;
   for (i = 0; i < n_seen; i++)
      if (!strncmp(seen[i], msg, sizeof(seen[i]) - 1))
         return;
   if (n_seen < 64)
      snprintf(seen[n_seen++], sizeof(seen[0]), "%s", msg);
   ALOG(RETRO_LOG_WARN, "GL type 0x%x severity 0x%x: %s\n", type, severity, msg);
}

static void enable_debug_output(void)
{
   void (*cb)(void (*)(GLenum, GLenum, GLuint, GLenum, GLsizei, const char *, const void *), const void *);
   void (*enable)(GLenum);
   if (!getenv("N64_ANGLE_DEBUG"))
      return;
   *(void **)&cb = (void *)angle_proc("glDebugMessageCallbackKHR");
   *(void **)&enable = (void *)angle_proc("glEnable");
   if (!cb || !enable)
      return;
   enable(GL_DEBUG_OUTPUT);
   enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
   cb(debug_cb, NULL);
}

bool angle_ctx_create(unsigned max_w, unsigned max_h, retro_log_printf_t log)
{
   static const EGLAttrib display_attribs[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
      EGL_NONE };
   static const EGLint config_attribs[] = {
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_NONE };
   static const EGLint pbuffer_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
   static const EGLint es_minor[] = { 2, 1, 0 };
   unsigned v;
   EGLConfig config = NULL;
   EGLint major = 0, minor = 0, n = 0;

   angle_log = log;
   if (a.active)
      return true;

   /* libGLESv2 first: libEGL finds it already loaded */
   if (!(a.gles = load_lib("libGLESv2.dylib")) || !(a.egl = load_lib("libEGL.dylib")))
      goto fail;

   EGLSYM(GetPlatformDisplay, "eglGetPlatformDisplay");
   EGLSYM(Initialize, "eglInitialize");
   EGLSYM(Terminate, "eglTerminate");
   EGLSYM(BindAPI, "eglBindAPI");
   EGLSYM(ChooseConfig, "eglChooseConfig");
   EGLSYM(CreatePbufferSurface, "eglCreatePbufferSurface");
   EGLSYM(CreateContext, "eglCreateContext");
   EGLSYM(DestroySurface, "eglDestroySurface");
   EGLSYM(DestroyContext, "eglDestroyContext");
   EGLSYM(MakeCurrent, "eglMakeCurrent");
   EGLSYM(GetProcAddress, "eglGetProcAddress");
   EGLSYM(GetError, "eglGetError");

   a.display = a.GetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, NULL, display_attribs);
   if (a.display == EGL_NO_DISPLAY || !a.Initialize(a.display, &major, &minor))
   {
      ALOG(RETRO_LOG_ERROR, "no Metal display (EGL error 0x%x)\n", a.GetError());
      goto fail;
   }
   a.BindAPI(EGL_OPENGL_ES_API);
   if (!a.ChooseConfig(a.display, config_attribs, &config, 1, &n) || n < 1)
   {
      ALOG(RETRO_LOG_ERROR, "no RGBA8/D24S8 ES3 config (EGL error 0x%x)\n", a.GetError());
      goto fail;
   }
   a.surface = a.CreatePbufferSurface(a.display, config, pbuffer_attribs);
   /* the newest ES 3.x ANGLE's Metal backend offers */
   for (v = 0; v < sizeof(es_minor) / sizeof(es_minor[0]) && !a.context; v++)
   {
      /* zero-filled new textures and renderbuffers, as the system OpenGL
       * gives them; GLideN64 relies on it (Metal leaves them undefined) */
      const EGLint context_attribs[] = {
         EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, es_minor[v],
         EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE, 1, EGL_NONE };
      a.context = a.CreateContext(a.display, config, EGL_NO_CONTEXT, context_attribs);
   }
   if (a.surface == EGL_NO_SURFACE || a.context == EGL_NO_CONTEXT
         || !a.MakeCurrent(a.display, a.surface, a.surface, a.context))
   {
      ALOG(RETRO_LOG_ERROR, "can't create an ES 3 context (EGL error 0x%x)\n", a.GetError());
      goto fail;
   }

   GLSYM(glGenFramebuffers); GLSYM(glDeleteFramebuffers); GLSYM(glBindFramebuffer);
   GLSYM(glGenRenderbuffers); GLSYM(glDeleteRenderbuffers); GLSYM(glBindRenderbuffer);
   GLSYM(glRenderbufferStorage); GLSYM(glFramebufferRenderbuffer);
   GLSYM(glCheckFramebufferStatus); GLSYM(glGetIntegerv); GLSYM(glPixelStorei);
   GLSYM(glBindBuffer); GLSYM(glReadPixels); GLSYM(glGetString); GLSYM(glGetStringi);

   /* The framebuffer GLideN64 treats as the screen (what RetroArch's would be) */
   a.glGenFramebuffers(1, &a.fbo);
   a.glBindFramebuffer(GL_FRAMEBUFFER, a.fbo);
   a.glGenRenderbuffers(1, &a.color);
   a.glBindRenderbuffer(GL_RENDERBUFFER, a.color);
   a.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, (GLsizei)max_w, (GLsizei)max_h);
   a.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, a.color);
   a.glGenRenderbuffers(1, &a.depth);
   a.glBindRenderbuffer(GL_RENDERBUFFER, a.depth);
   a.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)max_w, (GLsizei)max_h);
   a.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, a.depth);
   if (a.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
   {
      ALOG(RETRO_LOG_ERROR, "framebuffer %ux%u incomplete\n", max_w, max_h);
      goto fail;
   }
   a.glBindRenderbuffer(GL_RENDERBUFFER, 0);

   a.bgra = has_extension("GL_EXT_read_format_bgra");
   if (getenv("N64_ANGLE_DEBUG"))
   {
      GLint i, n = 0;
      a.glGetIntegerv(GL_NUM_EXTENSIONS, &n);
      for (i = 0; i < n; i++)
         ALOG(RETRO_LOG_INFO, "ext %s\n", (const char *)a.glGetStringi(GL_EXTENSIONS, (GLuint)i));
   }
   enable_debug_output();
   a.active = true;
   ALOG(RETRO_LOG_INFO, "EGL %d.%d, %s, %s, framebuffer %ux%u%s\n", major, minor,
         (const char *)a.glGetString(GL_VERSION), (const char *)a.glGetString(GL_RENDERER),
         max_w, max_h, a.bgra ? ", BGRA readback" : "");
   return true;

fail:
   angle_ctx_destroy();
   return false;
}

void angle_ctx_destroy(void)
{
   if (a.display)
   {
      if (a.context && a.MakeCurrent(a.display, a.surface, a.surface, a.context))
      {
         if (a.fbo) a.glDeleteFramebuffers(1, &a.fbo);
         if (a.color) a.glDeleteRenderbuffers(1, &a.color);
         if (a.depth) a.glDeleteRenderbuffers(1, &a.depth);
      }
      a.MakeCurrent(a.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (a.context) a.DestroyContext(a.display, a.context);
      if (a.surface) a.DestroySurface(a.display, a.surface);
      a.Terminate(a.display);
   }
   /* the libraries stay loaded: ANGLE keeps process-wide state (Metal
    * device, worker threads) that does not survive being unloaded */
   free(a.raw);
   free(a.frame);
   {
      void *gles = a.gles, *egl = a.egl;
      memset(&a, 0, sizeof(a));
      a.gles = gles;
      a.egl = egl;
   }
}

bool angle_ctx_active(void)
{
   return a.active;
}

void angle_ctx_make_current(void)
{
   if (a.active)
      a.MakeCurrent(a.display, a.surface, a.surface, a.context);
}

uintptr_t angle_ctx_get_framebuffer(void)
{
   return a.fbo;
}

const void *angle_ctx_read_frame(unsigned w, unsigned h, size_t *pitch)
{
   GLint prev_read = 0, prev_pack = 0, prev_align = 4;
   const size_t row = (size_t)w * 4;
   unsigned y, x;

   if (!a.active || !w || !h)
      return NULL;

   if (a.raw_size < row * h)
   {
      a.raw = (uint8_t *)realloc(a.raw, row * h);
      a.frame = (uint8_t *)realloc(a.frame, row * h);
      a.raw_size = a.frame_size = row * h;
   }

   a.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
   a.glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pack);
   a.glGetIntegerv(GL_PACK_ALIGNMENT, &prev_align);
   a.glBindFramebuffer(GL_READ_FRAMEBUFFER, a.fbo);
   a.glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
   a.glPixelStorei(GL_PACK_ALIGNMENT, 4);
   a.glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, a.bgra ? GL_BGRA_EXT : GL_RGBA,
         GL_UNSIGNED_BYTE, a.raw);
   a.glPixelStorei(GL_PACK_ALIGNMENT, prev_align);
   a.glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pack);
   a.glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);

   /* bottom-up rows -> top-down; RGBA -> XRGB8888 (B,G,R,X in memory) */
   for (y = 0; y < h; y++)
   {
      const uint8_t *src = a.raw + (size_t)(h - 1 - y) * row;
      uint8_t *dst = a.frame + (size_t)y * row;
      if (a.bgra)
         memcpy(dst, src, row);
      else
         for (x = 0; x < w; x++)
         {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = 0xff;
         }
   }

   *pitch = row;
   return a.frame;
}
