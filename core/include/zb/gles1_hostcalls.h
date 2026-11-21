// Index assignment for the GLES 1.1 entry points the NDK header declares and the GLES 2.0
// profile does not.
//
// tools/gen_stubs.py gives every name it does not share with an earlier library its own host
// call, and libGLESv1_CM.so is appended after libOpenSLES, so these 87 names occupy the
// contiguous block 492-578. The block is append-only like every other stub library: a new
// GLES1-only name must be appended by the generator, and this file must be updated in the
// same commit. tools/check_gles1_hostcalls.py enforces both.
//
// The 58 other names of GLES/gl.h are shared with GLES 2.0 and trap to their existing 0-141
// indices, which are already implemented.
#pragma once

#include <cstddef>
#include <cstdint>

namespace zb {

inline constexpr std::uint32_t kGl1HostCallFirst = 492u;
inline constexpr std::uint32_t kGl1HostCallLast = 578u;
inline constexpr std::size_t kGl1HostCallCount = 87u;
inline constexpr std::size_t kGl1PointerHostCallCount = 37u;

inline constexpr std::uint32_t ZB_GL1_HC_glAlphaFunc = 492u;
inline constexpr std::uint32_t ZB_GL1_HC_glAlphaFuncx = 493u;
inline constexpr std::uint32_t ZB_GL1_HC_glClearColorx = 494u;
inline constexpr std::uint32_t ZB_GL1_HC_glClearDepthx = 495u;
inline constexpr std::uint32_t ZB_GL1_HC_glClientActiveTexture = 496u;
inline constexpr std::uint32_t ZB_GL1_HC_glClipPlanef = 497u;
inline constexpr std::uint32_t ZB_GL1_HC_glClipPlanex = 498u;
inline constexpr std::uint32_t ZB_GL1_HC_glColor4f = 499u;
inline constexpr std::uint32_t ZB_GL1_HC_glColor4ub = 500u;
inline constexpr std::uint32_t ZB_GL1_HC_glColor4x = 501u;
inline constexpr std::uint32_t ZB_GL1_HC_glColorPointer = 502u;
inline constexpr std::uint32_t ZB_GL1_HC_glDepthRangex = 503u;
inline constexpr std::uint32_t ZB_GL1_HC_glDisableClientState = 504u;
inline constexpr std::uint32_t ZB_GL1_HC_glEnableClientState = 505u;
inline constexpr std::uint32_t ZB_GL1_HC_glFogf = 506u;
inline constexpr std::uint32_t ZB_GL1_HC_glFogfv = 507u;
inline constexpr std::uint32_t ZB_GL1_HC_glFogx = 508u;
inline constexpr std::uint32_t ZB_GL1_HC_glFogxv = 509u;
inline constexpr std::uint32_t ZB_GL1_HC_glFrustumf = 510u;
inline constexpr std::uint32_t ZB_GL1_HC_glFrustumx = 511u;
inline constexpr std::uint32_t ZB_GL1_HC_glGetClipPlanef = 512u;
inline constexpr std::uint32_t ZB_GL1_HC_glGetClipPlanex = 513u;
inline constexpr std::uint32_t ZB_GL1_HC_glGetFixedv = 514u;
inline constexpr std::uint32_t ZB_GL1_HC_glGetLightfv = 515u;
inline constexpr std::uint32_t ZB_GL1_HC_glGetLightxv = 516u;
inline constexpr std::uint32_t ZB_GL1_HC_glGetMaterialfv = 517u;
inline constexpr std::uint32_t ZB_GL1_HC_glGetMaterialxv = 518u;
inline constexpr std::uint32_t ZB_GL1_HC_glGetPointerv = 519u;
inline constexpr std::uint32_t ZB_GL1_HC_glGetTexEnvfv = 520u;
inline constexpr std::uint32_t ZB_GL1_HC_glGetTexEnviv = 521u;
inline constexpr std::uint32_t ZB_GL1_HC_glGetTexEnvxv = 522u;
inline constexpr std::uint32_t ZB_GL1_HC_glGetTexParameterxv = 523u;
inline constexpr std::uint32_t ZB_GL1_HC_glLightModelf = 524u;
inline constexpr std::uint32_t ZB_GL1_HC_glLightModelfv = 525u;
inline constexpr std::uint32_t ZB_GL1_HC_glLightModelx = 526u;
inline constexpr std::uint32_t ZB_GL1_HC_glLightModelxv = 527u;
inline constexpr std::uint32_t ZB_GL1_HC_glLightf = 528u;
inline constexpr std::uint32_t ZB_GL1_HC_glLightfv = 529u;
inline constexpr std::uint32_t ZB_GL1_HC_glLightx = 530u;
inline constexpr std::uint32_t ZB_GL1_HC_glLightxv = 531u;
inline constexpr std::uint32_t ZB_GL1_HC_glLineWidthx = 532u;
inline constexpr std::uint32_t ZB_GL1_HC_glLoadIdentity = 533u;
inline constexpr std::uint32_t ZB_GL1_HC_glLoadMatrixf = 534u;
inline constexpr std::uint32_t ZB_GL1_HC_glLoadMatrixx = 535u;
inline constexpr std::uint32_t ZB_GL1_HC_glLogicOp = 536u;
inline constexpr std::uint32_t ZB_GL1_HC_glMaterialf = 537u;
inline constexpr std::uint32_t ZB_GL1_HC_glMaterialfv = 538u;
inline constexpr std::uint32_t ZB_GL1_HC_glMaterialx = 539u;
inline constexpr std::uint32_t ZB_GL1_HC_glMaterialxv = 540u;
inline constexpr std::uint32_t ZB_GL1_HC_glMatrixMode = 541u;
inline constexpr std::uint32_t ZB_GL1_HC_glMultMatrixf = 542u;
inline constexpr std::uint32_t ZB_GL1_HC_glMultMatrixx = 543u;
inline constexpr std::uint32_t ZB_GL1_HC_glMultiTexCoord4f = 544u;
inline constexpr std::uint32_t ZB_GL1_HC_glMultiTexCoord4x = 545u;
inline constexpr std::uint32_t ZB_GL1_HC_glNormal3f = 546u;
inline constexpr std::uint32_t ZB_GL1_HC_glNormal3x = 547u;
inline constexpr std::uint32_t ZB_GL1_HC_glNormalPointer = 548u;
inline constexpr std::uint32_t ZB_GL1_HC_glOrthof = 549u;
inline constexpr std::uint32_t ZB_GL1_HC_glOrthox = 550u;
inline constexpr std::uint32_t ZB_GL1_HC_glPointParameterf = 551u;
inline constexpr std::uint32_t ZB_GL1_HC_glPointParameterfv = 552u;
inline constexpr std::uint32_t ZB_GL1_HC_glPointParameterx = 553u;
inline constexpr std::uint32_t ZB_GL1_HC_glPointParameterxv = 554u;
inline constexpr std::uint32_t ZB_GL1_HC_glPointSize = 555u;
inline constexpr std::uint32_t ZB_GL1_HC_glPointSizePointerOES = 556u;
inline constexpr std::uint32_t ZB_GL1_HC_glPointSizex = 557u;
inline constexpr std::uint32_t ZB_GL1_HC_glPolygonOffsetx = 558u;
inline constexpr std::uint32_t ZB_GL1_HC_glPopMatrix = 559u;
inline constexpr std::uint32_t ZB_GL1_HC_glPushMatrix = 560u;
inline constexpr std::uint32_t ZB_GL1_HC_glRotatef = 561u;
inline constexpr std::uint32_t ZB_GL1_HC_glRotatex = 562u;
inline constexpr std::uint32_t ZB_GL1_HC_glSampleCoveragex = 563u;
inline constexpr std::uint32_t ZB_GL1_HC_glScalef = 564u;
inline constexpr std::uint32_t ZB_GL1_HC_glScalex = 565u;
inline constexpr std::uint32_t ZB_GL1_HC_glShadeModel = 566u;
inline constexpr std::uint32_t ZB_GL1_HC_glTexCoordPointer = 567u;
inline constexpr std::uint32_t ZB_GL1_HC_glTexEnvf = 568u;
inline constexpr std::uint32_t ZB_GL1_HC_glTexEnvfv = 569u;
inline constexpr std::uint32_t ZB_GL1_HC_glTexEnvi = 570u;
inline constexpr std::uint32_t ZB_GL1_HC_glTexEnviv = 571u;
inline constexpr std::uint32_t ZB_GL1_HC_glTexEnvx = 572u;
inline constexpr std::uint32_t ZB_GL1_HC_glTexEnvxv = 573u;
inline constexpr std::uint32_t ZB_GL1_HC_glTexParameterx = 574u;
inline constexpr std::uint32_t ZB_GL1_HC_glTexParameterxv = 575u;
inline constexpr std::uint32_t ZB_GL1_HC_glTranslatef = 576u;
inline constexpr std::uint32_t ZB_GL1_HC_glTranslatex = 577u;
inline constexpr std::uint32_t ZB_GL1_HC_glVertexPointer = 578u;

struct Gl1HostCallInfo {
    std::uint32_t index;
    const char* name;
    bool has_pointer;
};

inline constexpr Gl1HostCallInfo kGl1HostCalls[kGl1HostCallCount] = {
    {492u, "glAlphaFunc", false},
    {493u, "glAlphaFuncx", false},
    {494u, "glClearColorx", false},
    {495u, "glClearDepthx", false},
    {496u, "glClientActiveTexture", false},
    {497u, "glClipPlanef", true},
    {498u, "glClipPlanex", true},
    {499u, "glColor4f", false},
    {500u, "glColor4ub", false},
    {501u, "glColor4x", false},
    {502u, "glColorPointer", true},
    {503u, "glDepthRangex", false},
    {504u, "glDisableClientState", false},
    {505u, "glEnableClientState", false},
    {506u, "glFogf", false},
    {507u, "glFogfv", true},
    {508u, "glFogx", false},
    {509u, "glFogxv", true},
    {510u, "glFrustumf", false},
    {511u, "glFrustumx", false},
    {512u, "glGetClipPlanef", true},
    {513u, "glGetClipPlanex", true},
    {514u, "glGetFixedv", true},
    {515u, "glGetLightfv", true},
    {516u, "glGetLightxv", true},
    {517u, "glGetMaterialfv", true},
    {518u, "glGetMaterialxv", true},
    {519u, "glGetPointerv", true},
    {520u, "glGetTexEnvfv", true},
    {521u, "glGetTexEnviv", true},
    {522u, "glGetTexEnvxv", true},
    {523u, "glGetTexParameterxv", true},
    {524u, "glLightModelf", false},
    {525u, "glLightModelfv", true},
    {526u, "glLightModelx", false},
    {527u, "glLightModelxv", true},
    {528u, "glLightf", false},
    {529u, "glLightfv", true},
    {530u, "glLightx", false},
    {531u, "glLightxv", true},
    {532u, "glLineWidthx", false},
    {533u, "glLoadIdentity", false},
    {534u, "glLoadMatrixf", true},
    {535u, "glLoadMatrixx", true},
    {536u, "glLogicOp", false},
    {537u, "glMaterialf", false},
    {538u, "glMaterialfv", true},
    {539u, "glMaterialx", false},
    {540u, "glMaterialxv", true},
    {541u, "glMatrixMode", false},
    {542u, "glMultMatrixf", true},
    {543u, "glMultMatrixx", true},
    {544u, "glMultiTexCoord4f", false},
    {545u, "glMultiTexCoord4x", false},
    {546u, "glNormal3f", false},
    {547u, "glNormal3x", false},
    {548u, "glNormalPointer", true},
    {549u, "glOrthof", false},
    {550u, "glOrthox", false},
    {551u, "glPointParameterf", false},
    {552u, "glPointParameterfv", true},
    {553u, "glPointParameterx", false},
    {554u, "glPointParameterxv", true},
    {555u, "glPointSize", false},
    {556u, "glPointSizePointerOES", true},
    {557u, "glPointSizex", false},
    {558u, "glPolygonOffsetx", false},
    {559u, "glPopMatrix", false},
    {560u, "glPushMatrix", false},
    {561u, "glRotatef", false},
    {562u, "glRotatex", false},
    {563u, "glSampleCoveragex", false},
    {564u, "glScalef", false},
    {565u, "glScalex", false},
    {566u, "glShadeModel", false},
    {567u, "glTexCoordPointer", true},
    {568u, "glTexEnvf", false},
    {569u, "glTexEnvfv", true},
    {570u, "glTexEnvi", false},
    {571u, "glTexEnviv", true},
    {572u, "glTexEnvx", false},
    {573u, "glTexEnvxv", true},
    {574u, "glTexParameterx", false},
    {575u, "glTexParameterxv", true},
    {576u, "glTranslatef", false},
    {577u, "glTranslatex", false},
    {578u, "glVertexPointer", true},
};

inline constexpr const Gl1HostCallInfo* gl1_host_call(std::uint32_t index) {
    if (index < kGl1HostCallFirst || index > kGl1HostCallLast) return nullptr;
    return &kGl1HostCalls[index - kGl1HostCallFirst];
}

inline constexpr bool is_gl1_host_call(std::uint32_t index) {
    return gl1_host_call(index) != nullptr;
}

inline constexpr const char* gl1_host_call_name(std::uint32_t index) {
    const Gl1HostCallInfo* info = gl1_host_call(index);
    return info == nullptr ? "?" : info->name;
}

}  // namespace zb
