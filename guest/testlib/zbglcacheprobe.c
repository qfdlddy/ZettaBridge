/* End-to-end guest probe verifying libGLESv2.so per-thread TLS state cache, glDelete* epoch
 * invalidation, and eglMakeCurrent host-epoch invalidation across real arm32 guest calls. */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stddef.h>

int zb_gl_cache_probe(void) {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) return 1;
    EGLint attribs[] = {EGL_RED_SIZE, 8, EGL_NONE};
    EGLConfig config = NULL;
    EGLint count = 0;
    if (!eglChooseConfig(display, attribs, &config, 1, &count) || count < 1) return 2;
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
    if (context == EGL_NO_CONTEXT) return 3;
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) return 4;

    /* Repeated identical state calls: each pair/triple must reach host exactly once. */
    glUseProgram(7);
    glUseProgram(7);
    glUseProgram(7);

    glActiveTexture(GL_TEXTURE0);
    glActiveTexture(GL_TEXTURE0);

    glBindTexture(GL_TEXTURE_2D, 55);
    glBindTexture(GL_TEXTURE_2D, 55);

    glBindBuffer(GL_ARRAY_BUFFER, 9);
    glBindBuffer(GL_ARRAY_BUFFER, 9);

    glEnable(GL_BLEND);
    glEnable(GL_BLEND);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glViewport(0, 0, 1280, 720);
    glViewport(0, 0, 1280, 720);

    glBindFramebuffer(GL_FRAMEBUFFER, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, 3);

    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(2);

    glStencilMask(0xFFu);
    glStencilMask(0xFFu);
    glStencilFuncSeparate(GL_FRONT_AND_BACK, GL_ALWAYS, 1, 0xFFu);
    glStencilFunc(GL_ALWAYS, 1, 0xFFu);
    glStencilOpSeparate(GL_FRONT_AND_BACK, GL_KEEP, GL_KEEP, GL_REPLACE);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glClearDepthf(1.0f);
    glClearDepthf(1.0f);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    /* VBO 9 is already bound: identical glVertexAttribPointer calls deduplicate in guest. */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (const void*)0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (const void*)0);

    /* Enable attrib 0 (already VBO-backed) and bind EBO so glDrawElements batches cleanly. */
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 12);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void*)0);

    const GLfloat u4[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(u4), u4);

    /* glUniform4fv batching test (queued and flushed before glDeleteProgram / glFinish). */
    glUniform4fv(0, 1, u4);

    /* glDeleteProgram bumps guest delete_epoch -> glUseProgram(7) must reach host,
     * while cap cache (GL_BLEND) remains intact until eglMakeCurrent. */
    glDeleteProgram(7);
    glEnable(GL_BLEND);
    glUseProgram(7);

    /* Second eglMakeCurrent bumps g_host_epoch -> subsequent cached calls must reach host. */
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) return 5;
    glUseProgram(7);
    glEnable(GL_BLEND);
    glFinish();
    return 0;
}
