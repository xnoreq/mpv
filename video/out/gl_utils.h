#ifndef MP_GL_UTILS
#define MP_GL_UTILS

#include "gl_context.h"

void glAdjustAlignment(GL *gl, int stride);
int glFmt2bpp(GLenum format, GLenum type);
void glUploadTex(GL *gl, GLenum target, GLenum format, GLenum type,
                 const void *dataptr, int stride,
                 int x, int y, int w, int h, int slice);
void glClearTex(GL *gl, GLenum target, GLenum format, GLenum type,
                int x, int y, int w, int h, uint8_t val, void **scratch);
void glDownloadTex(GL *gl, GLenum target, GLenum format, GLenum type,
                   void *dataptr, int stride);
struct mp_log;
void glCheckError(GL *gl, struct mp_log *log, const char *info);

struct mp_image;
struct mp_image *glGetWindowScreenshot(GL *gl);

#define GL_3D_RED_CYAN        1
#define GL_3D_GREEN_MAGENTA   2
#define GL_3D_QUADBUFFER      3

void glEnable3DLeft(GL *gl, int type);
void glEnable3DRight(GL *gl, int type);
void glDisable3D(GL *gl, int type);

// print a multi line string with line numbers (e.g. for shader sources)
// log, lev: module and log level, as in mp_msg()
void mp_log_source(struct mp_log *log, int lev, const char *src);

#endif
