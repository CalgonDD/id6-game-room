#pragma once

// The native QNX window is composited using alpha. PSP framebuffer alpha is
// game data, not window opacity. Only fix the final display buffer; retain
// offscreen alpha and restore every GL state changed here for the renderer.
static inline void QNXMakeBackbufferOpaque() {
    GLint framebuffer = 0;
    GLboolean mask[4] = {};
    GLfloat clear[4] = {};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (framebuffer != 0)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (scissor)
        glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    if (framebuffer != 0)
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
}
