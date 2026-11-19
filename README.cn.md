# ZettaBridge

**在仅支持 64 位的 ARM 手机上运行 32 位 Android 应用。**
一个源码可见（source-available）的 ARM32 -> ARM64 原生代码转译器与应用启动器。

[Read in English](README.md) | [Читать на русском](README.ru.md)

---

## 变更说明

针对 Snapdragon 7s Gen 4 进行适配与优化，相比上游 `ZettaBridge` 新增了以下修改：

- **音频与兼容性垫片（Shims）**
  - **OpenSL ES 音频桥接（`Phase 7c`）：** 提供 arm32 `libOpenSLES.so` 桩库（`guest/stubs/libOpenSLES.S`）与宿主后端（`HostOpenSl` / `AndroidOpenSlBackend`），通过宿主 64 位 OpenSL ES 驱动 FMOD/Unity 音频输出，并为单队列回调提供无锁快速路径。
  - **离线 Play Licensing 垫片（`PlayLicensingShim`）与广告视图隐藏（`AdHider`）：** 在无 Google Play 商店的插件沙盒中为依赖 LVL 授权校验的游戏合成本地 `ILicensingService` Binder/Proxy 响应，并支持可选隐藏内嵌广告 SDK 视图。
  - **加壳 ELF 与加载器修复：** 兼容伪造 `e_phentsize` 的加壳 `.so` 库（例如 `libRMS.so`）以及重叠 `PT_LOAD` 段的页权限处理（`elf_fixups.cpp`、`elf_loader.cpp`、`tools/fix_guest_lib.py`）。
- **图形（`GLES 2.0 / 3.0`）缓存与批处理**
  - **Guest 端每线程 GL 状态缓存与批处理缓冲区（`guest/stubs/libGLESv2_cache.c`）：** 61 个缓存/批处理包装函数可在已转译的 arm32 代码内直接过滤冗余 GL 状态调用（零 SVC 退出），并将非冗余的 void 调用（包括 `glUniformMatrix3fv`/`4fv`、`glUniform2i`/`2fv`、`glBindBufferBase`/`Range`、状态与绘制调用）写入每线程 2048 字的批处理缓冲区。
  - **Host 端 GL 快速路径（`host_gl.cpp`、`gl_manual.cpp`）：** 在 `GuestThread` 上缓存已校验的批处理缓冲区宿主指针，将动态状态过滤器失效与不可变的 EGL 上下文字符串/扩展缓存拆分开，并支持可选的 `FrameProbe` 帧耗时拆解统计。
- **核心转译器、系统调用、内存与工具链优化**
  - **JIT 内快速 SVC 与 JNI/Futex 路径（`process.cpp`、`syscalls.cpp`、`host_jni.cpp`）：** 高频系统调用（`clock_gettime`、`gettid`、`futex` wake 及非阻塞 `FUTEX_WAIT` `-EAGAIN` 快速失败、带缓存的 `read`/`pread64`）与 leaf GL/JNI 调用（含非 static 的 `GetField`/`SetField`）可直接在 Dynarmic 回调内完成，无需退出 `Jit::Run()`；纯整数系统调用跳过 `HostFpcrGuard`（`mrs fpcr`）。
  - **用户态 APK/OBB I/O 缓存（`guest_io.cpp`）：** 为 Unity/ZIP 读取提供无锁 fd 表、64 KiB 只读分块缓存与 mmap 路径。
  - **8 页批处理 `GuestMemory::accessible_span()`**、**`ZB_UNSAFE_FP`** 宽松 IEEE-754 JIT 浮点优化、**Android ADPF**（API 31+ `APerformanceHintManager`，位于 `cpu_tuning.cpp`）、**`-moutline-atomics`**（在 ARMv8.1+ 上自动启用 LSE 硬件原子指令）、**ThinLTO**（`ZB_ENABLE_LTO=ON`）、**16 KiB ELF 对齐**，以及 **`ZB_DISABLE_TELEMETRY`**（`ZB_NO_TELEMETRY=1`，`Release` 模式默认开启）。
- **Launcher 开关与设备端独立 APK 导出（`android/launcher/`）**
  - 按插件配置运行参数（`Fast FP`、`Unity MT` / `-force-gfx-mt`、`Hide ads`、`Precise faults`、`GL diagnostics`、`Application` 类覆盖）。
  - **设备端独立 APK 导出器（`ApkExporter`、`ApkSignerHelper`、`AxmlEditor`、`ArscIconFinder`、`StandaloneLaunchActivity`）：** 使用 `apksig` 直接在手机端将已导入的 32 位插件重打包为 v1/v2/v3 签名的独立 arm64 APK（含 OBB 打包、Manifest/图标修补、预修复 `.so`，以及可选的零遥测 `assets/zb-standalone/libzbridge.so`）。
- **新增宿主机单元测试：** 新增 `exclusive_aba_test`、`fast_svc_test`、`gl_state_cache_test`、`guest_io_apk_cache_test` 与 `opensl_chain_test`。
- **新增 Dynarmic 补丁：** 新增 `third_party/patches/dynarmic-0003-zbridge-all.patch`（初始化子模块后需与 `dynarmic-0001-thumb32-armv8.patch`、`dynarmic-0002-asimd-narrowing.patch` 一并 `git apply`）。

> **声明：** 以上所有修改与文档均由 Gemini 4、GLM 5.3 Flash 与 DeepSeek Flash 1017 完成与编写，不保证可用性与稳定性。

---

越来越多的新款手机 SoC（例如 Snapdragon 8 Elite）已彻底移除 AArch32 执行状态。仅包含 `armeabi` / `armeabi-v7a` 原生库的旧应用在这些设备上根本无法启动，且没有任何硬件级回退方案。

ZettaBridge 在**自身进程内部**运行此类应用：
- 应用的 Java/Kotlin 代码直接在手机正常的 64 位 ART 上原生运行；
- 仅将应用的 32 位原生代码转译为 AArch64。

无需 Root、无需定制 ROM、无需修改系统镜像。

> **状态：早期开发中。** 尚不适合普通终端用户日常使用。参见 [路线图](#路线图)。

## 2014 年的 arm32 游戏《Orange Roulette》运行在无 32 位 CPU 的手机上

| 开屏界面 | 主菜单 | 游戏画面 |
|---|---|---|
| ![Intro screen](images/screenshots/intro.jpg) | ![Main menu](images/screenshots/menu.jpg) | ![Gameplay](images/screenshots/gameplay.jpg) |

上方画面中的每一个像素均由转译后的 32 位 ARM 代码绘制：OnePlus 13（Snapdragon 8 Elite）没有 AArch32 执行状态，若没有 ZettaBridge 该游戏完全无法在其上启动。

## 工作原理

```
ZettaBridge 启动器（arm64 应用）
 |
 +-- 将 APK 作为插件导入，并在桌面固定快捷方式（无需真实安装）
 +-- :guest 进程
      +-- 插件 Java 代码运行在真实的 64 位 ART 上
      +-- libzbridge.so (arm64)
           +-- Dynarmic：A32/T32 -> AArch64 JIT，4 GiB 客体地址空间
           +-- 系统调用层：32 位 Linux ABI -> 64 位内核
           +-- 客体线程、信号处理、供 Java 调用方借用的 "carrier" 线程
           +-- JNI 桥接：合成的 32 位 JNIEnv / JavaVM
      +-- 真实的 arm32 Android 系统库（linker、libc、libm、libc++ 等）
```

- **原生代码由 [Dynarmic](https://github.com/Vita3K/dynarmic) 转译**（0BSD 协议），这是一个 ARM 动态重编译器，本项目附带了针对 ARMv8 Thumb-2 指令的小型本地补丁。
- **客体运行真实的 arm32 bionic。** Android linker、libc 等均来自 AOSP 系统镜像。ZettaBridge 转译 Linux 系统调用，而非逐个替换 libc 函数。
- **Java <-> 原生代码通过 JNI 桥接交互。**
  - 对 32 位库调用 `System.loadLibrary` 时，实际会加载一个极小的 arm64 代理库。
  - 代理库负责绑定客体的 `Java_*` 导出符号并运行其 `JNI_OnLoad`。
  - 客体原生代码通过合成的 32 位 `JNIEnv` 与 Java 通信。
- **在导入时自动修复旧原生库。** 会重写文本重定位（text relocations）与绝对路径 `DT_NEEDED`，因为现代 Android linker 会拒绝加载它们。

## 当前已实现功能

- **转译器核心。** 在 aarch64 Linux 上可通过 `zbrun` 运行，在真机上可运行于 Termux 及 ART 同进程内：
  - 通过真实 arm32 linker 运行静态与动态 arm32 Android 可执行文件；
  - 支持多线程、信号、C++ 异常、`dlopen`、内核用户辅助函数（kuser helpers）。
- **启动器。** 将 APK 作为插件导入并固定桌面快捷方式。已可直接作为插件运行 64 位应用，包括集成 Firebase/AdMob、Jetpack Compose 和 Flutter 的应用。
- **JNI 桥接。** 已实现并通过宿主机 Mock JVM 测试：
  - 客体 `JNIEnv`/`JavaVM`；
  - 多 Java 线程并发调用客体原生代码；
  - `RegisterNatives`；
  - 动态库加载与 `Java_*` 绑定。
- **GLES 2.0 / 3.0 透传、EGL 与 ANativeWindow。** 客体可在各自线程创建自己的 GL 上下文；约 490 个入口点由 Khronos 注册表自动生成。
- **已在手机上跑通的游戏。**
  - *Orange Roulette*（2014，Haxe/OpenFL）：菜单、实机游玩及声音均正常。
  - *Flappy Bird*（AndEngine）：可正常游玩。
  - **基于 `NativeActivity` 的 Unity 游戏**（2026 年 10 月）：*Lane Racer* 与 *Thomas Was Alone*（Unity 4.5，Mono）均可启动、渲染并支持触控操作。两者在运行一段时间后仍会退出进程，目前正在修复中，修复后将发布版本。
  - 现代 Flutter 应用可启动、运行 Dart 代码、输出帧并接收触控输入，但渲染尚不完整。

暂不支持：Vulkan 以及依赖真实包安装才能工作的功能。

## 路线图

| 里程碑 | 内容规划 |
|---|---|
| **v0.1** | 不自行绘图的 32 位原生库应用：使用旧原生库进行加密、图像处理、解析器、数据库的工具类应用。 |
| **v0.2** | OpenGL ES 透传与 Android 资源支持：以《Orange Roulette》为起点的简单 2D 游戏。 |
| **v0.3+** | 3D 游戏与性能优化：GL 调用批处理、更快浮点运算、JNI 快速路径 |
| 探索中 | 通过 [Box64](https://github.com/ptitSeb/box64) 运行 x86 / x86_64 客体 |

## 客观限制

- **性能开销。** 转译代码比原生执行慢：在 Snapdragon 8 Elite 上整数运算约慢 2 倍，内存拷贝约慢 3.5 倍，重度浮点循环会更慢。
- **3D 游戏难度更高但并非不可行。** 应用对系统的每一次调用（尤其是 OpenGL ES 调用）都要跨越转译边界，因此重度 3D 游戏初期速度较慢。让其可玩是项目目标，计划工作包括：
  - GL 调用批处理；
  - 更快的浮点处理；
  - 宿主侧 JNI 快速路径。
- **需要真实系统安装的应用超出范围：** 例如独立 UID 与系统权限、对其他应用的包可见性、系统账户、推送通知等。
- **部分应用会主动拒绝在其他应用内部运行**（Play Integrity / 克隆检测）。本项目不会绕过此类保护。

## 构建（开发用）

环境要求：
- aarch64 Linux、clang、CMake、Ninja；
- Boost 头文件；
- Android NDK r29。

```
git submodule update --init; git -C third_party/dynarmic apply ../patches/dynarmic-0001-thumb32-armv8.patch ../patches/dynarmic-0002-asimd-narrowing.patch
```
```
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++; ninja -C build/host
```
```
tools/extract_sysroot.sh; tools/build_guest.sh; ctest --test-dir build/host; tools/run_guest_tests.sh
```

`tools/extract_sysroot.sh` 会下载约 1.2 GB 的 AOSP GSI 镜像并将其中的 arm32 系统库提取到 `sysroot/`。Android 端 `libzbridge.so` 与启动器的构建说明详见 `CLAUDE.md` 和 `docs/`。

## 支持项目

ZettaBridge 由一名开发者独立编写。如果它帮你运行了需要的应用，欢迎通过 [Boosty](https://boosty.to/zailox) 支持后续开发。

## 参与贡献

项目仍处于早期阶段。现阶段最有价值的贡献是提交 Issue，列出你想运行的旧 32 位应用，并附上应用名称与版本号、APK 来源以及启动时的具体现象。

## 致谢

- **[@kxiserino](https://github.com/kxiserino)**：其 `fix/unity-startup` 分支及分析文章在真机上梳理出了完整的 Unity 启动链路，包括缺失的传感器 ABI、IL2CPP 路径映射、Boehm GC 的 stop-the-world 握手以及 Adreno 驱动公开的扩展列表。本项目实现与之独立，但这些发现大幅缩短了排查时间，其分支也同样严谨地指出了已知局限。
- **[@zzahkaboom24](https://github.com/zzahkaboom24)**：第一位在 Boosty 上赞助本项目的人，也是第一个 Star。
- 感谢在最初几周点亮 Star 或在 Boosty 上支持过本项目的每一个人。这类底层项目往往要面对漫长的一连串黑屏，知道有人在期待它，会成为很大的动力。

## 许可证

ZettaBridge 属于 **源码可见（source-available），而非开源（open source）**。本项目同时适用 **两份** 累加许可证（见 [LICENSE](LICENSE)）：**PolyForm Noncommercial 1.0.0** 与 **PolyForm Perimeter 1.0.1**。你可以出于非商业目的阅读、修改和分享本代码；但两份协议共同禁止以下两件事：利用本项目盈利，以及向他人提供竞争性产品（即使免费提供也算作竞争产品）。

任何形式的商业用途——包括出售、集成到产品中发布、预装到设备 ROM 中——均需向作者申请单独授权（可按需联系获取）。

代码贡献需签署版权转让协议；详见 [CONTRIBUTING.md](CONTRIBUTING.md)。

第三方代码保留其原有许可证，见 `third_party/README.md`。Dynarmic 采用 0BSD，Khronos 注册表文件采用 Apache-2.0。运行时使用的 AOSP 系统库采用 Apache-2.0，且不包含在本仓库中。
