# Part 6: the GLES 1.1 fixed-function pipeline (Phase 8)

Status: design approved 2026-11-21 by the user (option A of the three follow-ups offered
after the import-time symbol precheck). Builds on Part 4
(`2026-09-16-gles-assets-design.md`: the host-call handler pattern, guest pointer discipline,
no host pointer in the guest) and Part 3 (`2026-09-13-guest-system-boundary-design.md`:
transitions, carrier threads, guest memory).

## Goal

Two arm32 guests - Zombie Road Trip (`com.heyhouser.OrangeRoulette` is unrelated; this is the
cocos2d/NME engine shared by Zombie Road Trip and Punch Quest) - already load, run, upload
textures and issue roughly a thousand draw calls per run. Their frames are black because every
fixed-function call returns 0 from an empty stub: the model-view and projection matrices never
leave identity, and the vertex, colour and texture-coordinate client arrays are never installed.

After this phase every GLES 1.1 entry point the NDK declares is either an already-implemented
GLES 2.0 host call (the names the two profiles share) or is forwarded to the device's real
`libGLESv1_CM.so`. The guests render with the driver's own fixed-function pipeline.

## Why forward to the real driver instead of emulating ES1 on ES2

1. **The guest cannot be asked to change.** It calls `glPushMatrix`, `glOrthof`,
   `glVertexPointer` because that is how its engine renders.
2. **A real ES1 context already exists.** Both APKs call
   `setEGLContextClientVersion` from Java; every native library declares
   `NEEDED libGLESv1_CM.so` and none declares `libGLESv2.so` (`libcocos2d.so`, `libgame.so`,
   `libuikit.so` and the rest were inspected). The games are shipping titles, so their
   fixed-function calls must work on a stock device. The previous device run also proved that
   the *shared* calls already work through `libGLESv2.so` in that context (989
   `glDrawArrays`, 387 `glDrawElements`, 15 `glTexImage2D` reached the driver with no GL
   error).
3. **Emulation is open-ended.** Reproducing matrices, lighting, fog, texenv, clip planes,
   client arrays, attribute binding and a generated shader per fixed-function state on ES2 is
   a large, permanently incomplete project. The device driver already implements all of it,
   and it is the same implementation the guest would use natively.

Forwarding is therefore not a shortcut: it is the only design that can be complete.

## Measured surface

Verified on this machine on 2026-11-21 against NDK r29 headers, the two APKs and the committed
`core/src/gen/hostcalls.inc`.

| Fact | Value |
|---|---|
| functions declared in `GLES/gl.h` | 145 |
| names shared with `GLES2/gl2.h` and already implemented (they trap to indices 0-141) | 58 |
| GLES1-only names, already assigned indices 492-578 by `tools/gen_stubs.py` | 87 |
| of those, no pointer parameter | 50 |
| of those, at least one pointer parameter | 37 |
| GLES1-only names that also appear in the GLES 3.0 registry | 0 |

The 87 already exist as guest stubs: they trap, reach `Process::dispatch_stop`, are logged as
unimplemented and return 0. This phase changes only the host side.

App-visible demand:

- Zombie Road Trip report: 68,019 unimplemented calls over 13 names - `glPushMatrix` 13400,
  `glPopMatrix` 13400, `glMultMatrixf` 13110, `glVertexPointer` 8468, `glColorPointer` 8468,
  `glTexCoordPointer` 8178, `glEnableClientState` 1160, `glDisableClientState` 1160,
  `glTranslatef` 290, `glScalef` 290, `glMatrixMode` 38, `glLoadIdentity` 38, `glOrthof` 19.
- Union of ES1-only imports across both APKs: 19 names - the 13 above plus `glAlphaFunc`,
  `glTexEnvi`, `glPointSizePointerOES`, `glFrustumf`, `glRotatef`.

The remaining 68 names are implemented for completeness: a per-game subset would have to be
re-measured for every new guest, and the cost is one table row each.

## Architecture

**`HostGl` claims the range.** Indices 492-578 are added to the ranges `HostGl` already serves
(0-141, 228-331, 332-343). A separate chained handler was rejected for two reasons:

- the runtime report, the per-function census, the EGL-context/generation tracking and the
  trace switch all live in `HostGl::handle_host_call`, and every other append-only library
  (assets, windows, EGL, input, OpenSL) is a different subsystem - these are GL calls and
  belong in the same accounting;
- `HostGl::handle_host_call` flushes the guest's deferred ES2 batch before it dispatches.
  GLES1 calls interleave with batched ES2 calls (`glBindTexture`, `glTexParameteri`,
  `glEnable` are cached wrapper names), so a handler outside `HostGl` would let an ES1 state
  call overtake queued ES2 calls.

**`Gl1Backend` is the seam.** It mirrors `GlBackend`: one virtual per entry point taking host
types, a default body that records through `invoke()` so a host mock needs one function, and
no state of its own. It deliberately has no `set_error`: a rejection (a bad guest pointer)
happens before the backend and is queued through the existing `GlBackend::set_error`, so the
guest's `glGetError` sees it in the normal order.

**`Gl1DriverBackend` (Android only) is the real implementation.** It is the only file in the
tree that includes `<GLES/gl.h>`. It must not include `<GLES2/gl2.h>`, `<GLES3/gl3.h>` or
`<GLES/gl.h>` together with them: the headers redeclare the shared entry points and the
translation unit will not compile. Its overrides are one-line forwards, like
`gl_driver_backend_overrides.inc`.

**Headless and host builds keep working.** `HostGl` takes the GL1 backend with a setter, so
every existing construction site (tests, `GuestGlEngine` without GL) compiles unchanged. With
no backend set, a GL1 call is rejected once with `GL_INVALID_OPERATION` and logged, which is
visible in the report instead of silently returning 0.

## Marshaling rules

Every GLES 1.1 parameter is 32 bits or narrower and is passed in one AAPCS32 word, so argument
index equals guest word index (positions 4 and above come from the guest stack). `GLfloat`
words are bit-preserved, `GLfixed`/`GLclampx` are signed 32-bit integers passed through
unchanged, and `GLubyte` parameters are promoted by the compiler and truncated to 8 bits.

| Rule | Functions |
|---|---|
| no pointer: pass the words | 50 names |
| 16 floats in | `glLoadMatrixf`, `glLoadMatrixx`, `glMultMatrixf`, `glMultMatrixx` |
| 4 elements in | `glClipPlanef`, `glClipPlanex` |
| 4 elements out | `glGetClipPlanef`, `glGetClipPlanex` |
| pname-sized out | `glGetFixedv`, `glGetLightfv`, `glGetLightxv`, `glGetMaterialfv`, `glGetMaterialxv`, `glGetTexEnvfv`, `glGetTexEnviv`, `glGetTexEnvxv`, `glGetTexParameterxv` |
| pname-sized in | `glFogfv`, `glFogxv`, `glLightModelfv`, `glLightModelxv`, `glLightfv`, `glLightxv`, `glMaterialfv`, `glMaterialxv`, `glPointParameterfv`, `glPointParameterxv`, `glTexEnvfv`, `glTexEnviv`, `glTexEnvxv`, `glTexParameterxv` |
| client array | `glColorPointer`, `glNormalPointer`, `glTexCoordPointer`, `glVertexPointer`, `glPointSizePointerOES` |
| guest pointer out | `glGetPointerv` |

Result counts by pname:

| pname | count |
|---|---|
| `GL_MODELVIEW_MATRIX`, `GL_PROJECTION_MATRIX`, `GL_TEXTURE_MATRIX` | 16 |
| `GL_CURRENT_NORMAL` | 3 |
| `GL_CURRENT_COLOR`, `GL_CURRENT_TEXTURE_COORDS`, `GL_BLEND_COLOR`, `GL_COLOR_CLEAR_VALUE`, `GL_COLOR_WRITEMASK`, `GL_SCISSOR_BOX`, `GL_VIEWPORT`, `GL_LIGHT_MODEL_AMBIENT`, `GL_FOG_COLOR` | 4 |
| `GL_ALIASED_POINT_SIZE_RANGE`, `GL_ALIASED_LINE_WIDTH_RANGE`, `GL_POINT_SIZE_RANGE`, `GL_SMOOTH_LINE_WIDTH_RANGE`, `GL_DEPTH_RANGE`, `GL_MAX_VIEWPORT_DIMS` | 2 |
| `GL_COMPRESSED_TEXTURE_FORMATS` | `GL_NUM_COMPRESSED_TEXTURE_FORMATS`, queried from the driver |
| everything else | 1 |

Per-function overrides: `glGetLightfv`/`xv` - 4 for `GL_AMBIENT`, `GL_DIFFUSE`,
`GL_SPECULAR`, `GL_POSITION`; 3 for `GL_SPOT_DIRECTION`; 1 otherwise.
`glGetMaterialfv`/`xv` - 4 for `GL_AMBIENT`, `GL_DIFFUSE`, `GL_SPECULAR`, `GL_EMISSION`,
`GL_AMBIENT_AND_DIFFUSE`; 1 for `GL_SHININESS`.
`glGetTexEnvfv`/`iv`/`xv` - 4 for `GL_TEXTURE_ENV_COLOR`, 1 otherwise.
`glGetTexParameterxv` - 4 for `GL_TEXTURE_BORDER_COLOR` (0x1004), 1 otherwise.
Inputs follow the same tables: `glFogfv` 4 for `GL_FOG_COLOR`; `glLightModelfv` 4 for
`GL_LIGHT_MODEL_AMBIENT`; `glPointParameterfv` 3 for `GL_DISTANCE_ATTENUATION` (0x8129);
`glMaterialfv`/`xv` 4 or 1 as for the getters; `glLightfv`/`xv` as for the getters.

Client arrays. The guest address is translated to `guest_base + address` after a one-page
readability check (one byte, because the length is only known at draw time) and passed to the
driver, which reads it when `glDrawArrays`/`glDrawElements` runs. The 32-bit guest address is
remembered per array for `glGetPointerv`, which is answered by the bridge and never reaches the
driver: the driver would return the 64-bit host pointer, which cannot be stored in a guest
`void*` slot.

Nothing is copied and nothing is emulated: the driver holds the fixed-function state.

## The shared ES2 handlers this phase extends

`gl_pname_count` (`core/src/gl/gl_manual.cpp`) currently returns 1 for anything it does not
know. In an ES1 context `glGetFloatv(GL_MODELVIEW_MATRIX)` asks for 16 floats, so the current
code validates 4 bytes and the driver writes 64 - a silent guest-memory overwrite. The table
above is added there, so `glGetFloatv`, `glGetIntegerv` and `glGetBooleanv` are correct for
both profiles.

`glDrawArrays`/`glDrawElements` need no change: with no GLES 2.0 client arrays enabled they
forward the guest index pointer and the draw untouched, which is exactly what an ES1 context
needs (it already works on the device).

## Threading, errors and diagnostics

- Handlers run inline on the calling host thread, exactly like Phase 5. On the device that
  thread is the one whose EGL context is current (`gl-egl-context-current: yes` was observed
  with the real driver in the Phase 5 run). No cross-thread dispatch and no queue.
- Rejections are limited to pointer checks and a missing backend. They set the shared pending
  error, are counted as `gl-rejections` and are visible to the guest.
- The first GL1 call records `gl-context: <GL_VERSION>` once in the runtime report, so the ES1
  assumption is verifiable from a single device run without logcat.
- GL1 calls count in `gl-calls`, in `gl-first-call` and in the per-function census. The census
  slot limit moves from 512 to 640 so indices 492-578 are counted rather than dropped, and the
  census resolves names through the same two-range helper, so a GL1 entry is never printed as
  `?=N`.
- **The GL visibility diagnostics (`gl_diagnostics.cpp`) are OpenGL ES 2.0+ only.** They query
  ES 2.0 entry points (`glCheckFramebufferStatus`, `glReadPixels` of `GL_RGBA`, uniform and
  pixel-store queries). An ES 1.1 game still calls the shared names - `glClear`, `glDrawArrays`,
  `glViewport` - which reach the same handler, and on the device
  `glCheckFramebufferStatus(GL_FRAMEBUFFER)` inside a GLES 1.1 context crashed the driver at
  address 0, killing Punch Quest on its first draw. The profile is therefore decided once per
  `GlBackend` from `glGetString(GL_VERSION)` and cached: only `OpenGL ES 2`/`OpenGL ES 3` prefixes
  run the diagnostics, everything else skips them and records
  `gl-diagnostics: <GL_VERSION> profile skip` once, so an absent set of `gl-*` visibility lines is
  explained by the report itself. A null version (no current context yet) is not cached, so the
  first call on a live context decides.

## Testing

- `tools/check_gles1_hostcalls.py` (ctest `gles1_hostcalls_check`): the 87 committed rows are
  exactly indices 492-578, exactly the `libGLESv1_CM.so` rows of `hostcalls.inc`, every name is
  declared by `GLES/gl.h`, and no name is declared by `GLES2/gl2.h`. Runs on any machine with
  the NDK, like `gen_gles_check`.
- `gles1_marshal_test` with `MockGles1`: all 50 pointerless functions, a six-argument call read
  from the guest stack, float bit preservation, fixed-point pass-through, `GLubyte`
  truncation, every pointer rule (fixed sizes, pname sizes, client arrays), out-of-bounds and
  unreadable pointers rejected before the backend, `glGetPointerv` returning the original guest
  address, and `glGetFloatv(GL_MODELVIEW_MATRIX)` validating 16 elements.
- Device: Zombie Road Trip, then Punch Quest.

## Acceptance

The device run shows a rendered first screen for Zombie Road Trip, and the runtime report
shows `unimplemented-host-calls: 0`, no `gl-rejections`, a `gl-context:` line naming an
OpenGL ES 1.x context, and `glOrthof`/`glPushMatrix` present in the census.

## Non-goals

- The 133 platform names our guest `libGLESv1_CM.so` still does not export
  (`tools/platform_exports/libGLESv1_CM.gap.txt`): all OES/QCOM/IMG extensions.
- ES1-on-ES2 emulation.
- Batching GLES1 calls. The design keeps the same one-call-one-trap shape so `HostGl`'s existing
  batch could later cover ES1 too.

## Risks

1. **A guest that mixes shaders and fixed function in one ES2 context** (`libquartzcore.so`
   imports `glCreateShader` and `glVertexPointer`) gets `GL_INVALID_OPERATION` from the driver
   for the fixed-function calls. That is the driver's answer, not ours; it appears as
   `gl-first-error` and is otherwise unchanged behavior for a guest that was already broken on
   a real device.
2. **Client-array lifetime.** The driver retains a host pointer to guest memory until the array
   is replaced. If a guest unmaps a live array the next draw reads unmapped host memory. The
   guest cannot see the host pointer, and ES1 applications keep arrays mapped across draws;
   this is noted rather than guarded, because guarding would require copying every array at
   draw time (a later performance phase).
3. **Driver reads past the validated byte.** The driver reads as much as the array layout
   implies, exactly as it would with a native pointer. Only the first byte is checked, because
   the length is not known at `glVertexPointer` time.
4. **`glGetPointerv` shadow state is per thread.** GL contexts are per thread in this runtime
   (the same assumption the ES2 client-array shadow already makes); a context used from two
   threads without rebinding arrays is not supported, as before.
