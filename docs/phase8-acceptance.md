# Phase 8 acceptance: the GLES 1.1 fixed-function pipeline

Date: 2026-11-21. Device: MuMu emulator, Android 15 (API 35) x86_64 with houdini; `arm64-v8a`
`libzbridge.so` runs translated, so fault PCs from a tombstone are approximate. The reports below
were read with `run-as com.zettabridge.launcher cat files/zb-runtime-report.txt`.

Design: `docs/superpowers/specs/2026-11-21-gles1-fixed-pipeline-design.md`.
Plan: `docs/superpowers/plans/2026-11-21-phase8-gles1.md`.

## What shipped

- Host-call indices 492-578 (87 rows of `core/src/gen/hostcalls.inc`) are claimed by `HostGl`
  and dispatched through the new `Gl1Backend` seam to the device's real `libGLESv1_CM.so`.
  Nothing about the fixed-function pipeline is emulated on GLES 2.0.
- All 87 entry points are functional: 50 pointerless, 37 with bounds-checked pointers, including
  the five client-array setters, the pname-sized `glGet*` outputs and the six-argument
  `glOrthof`/`glFrustumf` calls read from the guest stack.
- `glGetPointerv` is answered by the bridge from the remembered 32-bit guest address and never
  reaches the driver, so the guest never sees a host pointer.
- The shared pname table in `gl_manual.cpp` gained the ES1 result sizes, so
  `glGetFloatv(GL_MODELVIEW_MATRIX)` validates 64 bytes instead of 4.
- The per-function census covers 492-578 (slot limit 512 -> 640) and resolves names through the
  two-range helper, so GL1 entries are named instead of printed as `?=N`.
- The GL visibility diagnostics now run only on an OpenGL ES 2.0+ context (see the defect below).

## Defect found and fixed during acceptance

Punch Quest crashed on its first draw with `SIGSEGV` at address 0 inside the driver. The
unwound caller was `gl_diagnostics.cpp`'s `sample()`, at
`glCheckFramebufferStatus(GL_FRAMEBUFFER)`; the crash reproduced at the identical pc for Zombie
Road Trip once `diagnostics=true`, and disappeared for Punch Quest once `diagnostics=false`
(28,520 GL calls and a correct title screen instead of 82 calls and a crash). The diagnostics
are written for ES 2.0 and query ES 2.0-only entry points, but an ES 1.1 guest still calls the
shared names (`glClear`, `glDrawArrays`, `glViewport`) that reach the same handler.

Fix: the context profile is decided once per `GlBackend` from `glGetString(GL_VERSION)` and
cached; only `OpenGL ES 2`/`OpenGL ES 3` prefixes run the diagnostics. Everything else skips them
and records one line, so the absence of `gl-sample-draw-*`/`gl-draws` lines is explained by the
report itself. A null version (no current context yet) is not cached.

## Zombie Road Trip (`com.noodlecake.zombieroadtrip`)

Report, `diagnostics=true`, at the main menu:

```text
unimplemented-host-calls: 0
guest-exit: (none)
gl-calls: 67484
gl-egl-context-current: yes
gl-first-error: (none)
gl-diagnostics: OpenGL ES 1.1 v334 R profile skip
gl-context: OpenGL ES 1.1 v334 R
gl-census-1: glBindTexture=37 glBlendFunc=3 glClear=36 glClearColor=3 glDisable=41
             glDrawArrays=31 glEnable=39 glGenTextures=4 glGetError=2 glGetIntegerv=5
             glIsTexture=33 glPixelStorei=2 glTexImage2D=4 glTexParameteri=16 glViewport=9
             glColorPointer=31 glDisableClientState=99 glEnableClientState=110 glLoadIdentity=18
gl-census-2: glMatrixMode=14 glMultMatrixf=98 glOrthof=7 glPopMatrix=130 glPushMatrix=133
             glScalef=33 glTexCoordPointer=31 glVertexPointer=31
egl-first-error: (none)
```

After a tap on RIDE the loading screen renders and `gl-calls` reaches 373,724 with
`gl-first-error: (none)` and `guest-exit: (none)`.

Screens: the main menu (title art, side panel, RIDE button) and the "Please Wait... Preparing
scenery" loading screen, both fully rendered with GL1 fixed-function text.

`egl-swaps: 0` and `egl-thread-mismatch: current=0 gl=<tid>` are the known false alarms of a
Java `GLSurfaceView` that creates its own context; the frames are presented.

## Punch Quest (`com.noodlecake.punchquest`)

Report, `diagnostics=true` - the configuration that crashed before the fix:

```text
unimplemented-host-calls: 0
guest-exit: (none)
gl-calls: 12617
gl-egl-context-current: yes
gl-diagnostics: OpenGL ES 1.1 v334 R profile skip
gl-context: OpenGL ES 1.1 v334 R
gl-census-1: glBindTexture=37 glBlendFunc=3 glClear=36 glClearColor=3 glDisable=41
             glDrawArrays=31 glEnable=39 ...
```

Screen: the full title screen - logo, skeleton background, and the Stats/Options, Shop, More
Games and START panel - rendered and interactive.

## Topsoil (2026-11-21): runs and takes touch

`com.kneeko.topsoil` 1.2.9 (Haxe luxe on SDL2, armeabi-v7a, GLES 3.0) renders its tutorial
screen and responds to touch. Three fixes took it from a null-engine crash to a playable intro,
each found from one device report or one disassembly:

- **Guest `dlopen("/system/lib/libOpenSLES.so")`.** Its OpenAL probe (`libtopsoil.so`
  `alc_opensles_probe`, around `0x5c5e40`) calls `stat()` and then `dlopen(RTLD_NOW)` on that
  literal path, and dereferences a null engine on failure at `0x5ab848`. `translate_path` used to
  fall through to the device's own file, which is host architecture, so the guest loader met an
  x86 library. Missing guest libraries now resolve in `LibraryRuntimeOptions::guest_library_dirs`
  (`<files>/zb/guest/lib`, then the plugin's `lib/`). After the fix the report shows
  `opened-N: .../zb/guest/lib/libOpenSLES.so`, the engine, mixer and player go `ok`, and
  `audio-stream` enqueues buffers.
- **Wrong result type on a JNI call.** `AppActivity.setNotification(...)` is `void`, and the lime
  layer called it with `CallIntMethodA`. The bridge treated that as a fatal misuse, so the run
  ended at `jni-fatal: CallMethodA: result type I does not match the method shorty VLLLI`.
  `CallMethodA` now calls with the real shorty and returns a neutral zero, recording one
  `jni-result-mismatch` line.
- **A pending Java exception killed the process.** That `setNotification` call really throws on
  API 31+ (`IllegalArgumentException`: `PendingIntent` needs `FLAG_IMMUTABLE`/`FLAG_MUTABLE`),
  because the guest runs with the launcher's `targetSdk`. The guest never checks, and its next
  JNI call hit CheckJNI: `JNI NewStringUTF called with pending exception` aborted the `:guest`
  process. The bridge now holds the exception aside for the duration of each host call and
  re-throws it after (`jni-exception-deferred`), which is how release ART behaves.

Device evidence with all three fixes, OnePlus 13 / MuMu, `am start ... zbplugin://com.kneeko.topsoil`:

```text
guest-exit: (none)                         unimplemented-host-calls: 0
gl-calls: 254548                           egl-swaps: 595
gl-diagnostics: OpenGL ES 3.2 v334 R profile on
gl-first-error: (none)                     gl-shaders: ok=6 failed=0
guest-log: luxe 1.0.0-alpha.3 / OpenGL ES 3.0 / Adreno 640
jni-result-mismatch: setNotification(...)V returns V, asked I
jni-exception-deferred: DeleteLocalRef while java.lang.IllegalArgumentException was pending
```

Screen: the tutorial page ("Let's plant these in your garden!") with the seed packets, the garden
bed and the menu button; tapping a seed packet plants it in the bed, so SDL touch and the guest's
input path work. The game's own metrics request fails with `EOF` (its server is long gone) and
does not affect the run.

Regressions with the same APK: Punch Quest title screen (`gl-calls` 62,109, `diagnostics=true`,
`profile skip`) and Zombie Road Trip menu (`gl-calls` 123,249).

## Topsoil's own notification (2026-11-21): relayed through the launcher

The store's "notify me" bonus-plays reminder is the one part of the game that is itself Java code
on ART: `AppActivity.setNotification(String id, String title, String text, int seconds)` builds
`PendingIntent.getActivity(ctx, 0, intent, 0x08000000)` - no `FLAG_IMMUTABLE` - which throws
`IllegalArgumentException` on API 31+ before any `Notification.Builder` exists, posts with no
`NotificationChannel` (dropped on API 26+) and names a plugin drawable for its small icon. The
method has no Java caller; only `libtopsoil.so` calls it through JNI, so the bridge answers the
whole call instead of trying to hook its framework calls.

| Layer | Change |
|---|---|
| `core/src/jni/host_jni_internal.h`, `host_jni_objects.cpp` | `MethodHook::LegacyNotification` on `setNotification:(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V` |
| `core/src/jni/host_jni_values.cpp` | `CallMethodA` answers the hook before the result-type mismatch path |
| `core/src/jni/host_jni_compat.cpp` | `post_legacy_notification`: receiver must be `android.app.Activity`, then `ZBridge.notifyGuest`; any failure clears the exception and falls back to the guest method |
| `android/launcher/.../core/ZBridge.java` | `notifyGuest`/`postGuestNotification`, channel `zb.plugin` (`IMPORTANCE_HIGH`), `ic_popup_reminder`, launch intent, `FLAG_IMMUTABLE` |
| `android/launcher/.../NotificationRelayReceiver.java` | main-process receiver that posts when an `AlarmManager` alarm fires |
| `AndroidManifest.xml`, `LibraryActivity.java` | `POST_NOTIFICATIONS` permission, requested once on API 33+ |

Trigger path and results on MuMu (API 35), `am start ... zbplugin://com.kneeko.topsoil`:
pause menu (`180 1465`) -> store `+` (`845 85`) -> `bonus plays` (`450 833`) -> `notify me`
(`450 1490`, twice: the first tap clears the opt-in, the second sets it).

```text
jni-notification-relay: id 1 delay 80772s
dumpsys alarm: tag=*walarm*:com.zettabridge.launcher/.NotificationRelayReceiver
               type=ELAPSED_WAKEUP origWhen=+22h26m8s760ms
dumpsys notification: pkg=com.zettabridge.launcher user=UserHandle{0} id=1
               channel=zb.plugin importance=4 flags=AUTO_CANCEL
```

The shade shows "Bonus plays refilled! / Your plays can now be claimed." A diagnostic build clamped
the delay to 15 s to watch the relay fire (`origWhen=-10s`, then the posted record above); the
shipped build keeps the guest's own ~22 h delay, so the notification is scheduled rather than
already visible. A broadcast to the receiver from `adb shell am broadcast` is dropped because it is
`exported=false`, which is the intended protection.

Regressions with the same APK:

```text
Punch Quest        gl-calls 53313    egl-swaps 0     guest-exit: (none)   unimplemented-host-calls: 0
                   gl-diagnostics: OpenGL ES 1.1 v334 R profile skip
Zombie Road Trip   gl-calls 244273   egl-swaps 0     guest-exit: (none)   unimplemented-host-calls: 0
```

Both still render their title/garage screens, so the extra JNI hook does not disturb GLES 1.1
guests or their own notification-free paths.

## Verification performed

```text
tools/gen_gles.py --check, gen_jni/gen_egl/gen_ndk/gen_gl1_driver --check   PASS
tools/check_gles1_hostcalls.py                                             PASS
ninja -C build/android-arm64                                              PASS
bash tools/build_guest.sh                                                 PASS
tools/make_launcher_bundle.sh + check_launcher_bundle.py                   PASS (8.2 MiB)
gradlew assembleLauncherDebug                                             PASS
```

APK installed for the runs above: `app-launcher-debug.apk`, 11,789,259 bytes,
SHA-256 `9ee8a3eae3af8194c6ae1e8a88253a96cb1062e1ba6cd061ceffe05a56187ce9`. The preceding build
(`0be5b8e9...`) differed only in the linkage of the new diagnostics helper and produced the same
report lines.

The host suite cannot run in this checkout (no host C++ toolchain), so the new
`gles1_marshal_test` ES1 diagnostics case and the two updated diagnostics tests were compiled
only (`clang -fsyntax-only`) and are covered functionally by the device runs above.

## Device state left behind

- Punch Quest: `diagnostics=true` (its original value).
- Zombie Road Trip: `diagnostics=false` (its original value).

## Open follow-ups

- Topsoil runs and takes touch (see the section above); 133 OES/QCOM/IMG `libGLESv1_CM.so` names
  remain in `tools/platform_exports/libGLESv1_CM.gap.txt`.
- A guest's own `dlopen` failure is still not recorded in the runtime report.
- Only the guarded `setNotification(String,String,String,I)V` shape is relayed. A guest with a
  different old notification helper still meets ART's own behaviour (exception survived and
  reported by `jni-exception-deferred`, but no notification).
