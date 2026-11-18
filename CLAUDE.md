# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.
本文件为 Claude Code（claude.ai/code）在本仓库中工作时提供指引。

## Overview / 项目概览

FlightGear 二次开发 (secondary development) workspace. This is **not** a single project — it is a monorepo-style checkout of four related C++17 CMake projects that build in a specific order:
FlightGear 二次开发工作区。这里**不是**单个项目，而是四个相关的 C++17 CMake 项目的 monorepo 式检出，它们需要按特定顺序编译：

| Directory / 目录 | Project / 项目 | Role / 作用 | License / 许可证 |
|---|---|---|---|
| `simgear-2024.1.1/` | SimGear 2024.1.1 | Foundation library (property tree, Nasal, scene graph, math, IO, XML) / 基础库（属性树、Nasal、场景图、数学、IO、XML） | LGPL |
| `flightgear-2024.1.1/` | FlightGear 2024.1.1 | The flight simulator itself / 飞行模拟器本体 | GPL |
| `terragear-newconstr/` | TerraGear | Scenery-generation CLI tools (terrain preprocessing + tile assembly) / 地形生成命令行工具（地形预处理 + 瓦片拼装） | GPL |
| `fgms-0.13.1/` | FGMS 0.13.1 | Standalone multiplayer server / 独立联机服务器 | — |

`zips/` holds the downloaded source archives these were extracted from; it is git-ignored.
`zips/` 存放解压出上述项目的源码压缩包，已被 git 忽略。

## Build / 构建

All four projects build out-of-source with CMake (C++17 required). **Build SimGear first and install it**, then FlightGear/TerraGear discover it via `CMAKE_PREFIX_PATH` or `CMAKE_INSTALL_PREFIX`.
四个项目均使用 CMake 进行源外构建（要求 C++17）。**先编译并安装 SimGear**，随后 FlightGear/TerraGear 通过 `CMAKE_PREFIX_PATH` 或 `CMAKE_INSTALL_PREFIX` 来发现它。

```bash
# SimGear / 先编译 SimGear
cmake -S simgear-2024.1.1 -B build-simgear -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-simgear --config RelWithDebInfo
cmake --install build-simgear --prefix <install-prefix>

# FlightGear (needs SimGear + OpenSceneGraph + Boost et al. on the search path)
# FlightGear（需要 SimGear + OpenSceneGraph + Boost 等位于搜索路径上）
cmake -S flightgear-2024.1.1 -B build-fg -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_PREFIX_PATH="<install-prefix>"
cmake --build build-fg --config RelWithDebInfo
```

- On **Windows**, dependencies are discovered via the `MSVC_3RDPARTY_ROOT` / `3rdParty` layout — see [flightgear-2024.1.1/README.msvc](flightgear-2024.1.1/README.msvc).
  在 **Windows** 上，依赖通过 `MSVC_3RDPARTY_ROOT` / `3rdParty` 目录结构发现 —— 见 [flightgear-2024.1.1/README.msvc](flightgear-2024.1.1/README.msvc)。
- FlightGear ships `CMakePresets.json` (Ninja Multi-Config; binary dir `${sourceParentDir}/fgbuild`, install dir `${sourceParentDir}/dist`), so `cmake --preset configure-base` also works.
  FlightGear 自带 `CMakePresets.json`（Ninja Multi-Config；二进制目录 `${sourceParentDir}/fgbuild`，安装目录 `${sourceParentDir}/dist`），因此 `cmake --preset configure-base` 也可用。
- All generated `build*/`, `fgbuild/`, and `dist/` directories are git-ignored at the root.
  所有生成的 `build*/`、`fgbuild/`、`dist/` 目录均在根目录被 git 忽略。

## Tests / 测试

FlightGear has a CTest-integrated test suite in [flightgear-2024.1.1/test_suite/](flightgear-2024.1.1/test_suite/), enabled by default (`ENABLE_AUTOTESTING=ON`). It builds a single runner binary, `fgfs_test_suite`, and registers named suites:
FlightGear 在 [flightgear-2024.1.1/test_suite/](flightgear-2024.1.1/test_suite/) 中集成了 CTest 测试套件，默认启用（`ENABLE_AUTOTESTING=ON`）。它构建一个运行器二进制 `fgfs_test_suite`，并注册若干具名套件：

```bash
# run everything / 运行全部
ctest --test-dir build-fg

# run one named suite (name from add_test in test_suite/CMakeLists.txt) / 运行单个具名套件（名称见 test_suite/CMakeLists.txt 的 add_test）
ctest --test-dir build-fg -R FlightplanUnitTests
```

The runner selects suites by type flag: `--ctest -u <Suite>` (unit), `-s <Suite>` (system), `-m <Suite>` (SimGear module).
运行器通过类型标志选择套件：`--ctest -u <Suite>`（单元测试）、`-s <Suite>`（系统测试）、`-m <Suite>`（SimGear 模块测试）。

## Architecture / 架构

**Dependency direction:** SimGear is the leaf library; FlightGear and TerraGear depend on it. FGMS vendors its own copies (`fgms/src/simgear`, `plib`, `libcli`, `libmsc`) and does not link the workspace SimGear.
**依赖方向：** SimGear 是底层库；FlightGear 和 TerraGear 依赖它。FGMS 自带其依赖副本（`fgms/src/simgear`、`plib`、`libcli`、`libmsc`），不链接本工作区的 SimGear。

**FlightGear is decomposed into ~30 link-order-sensitive module libraries.** [flightgear-2024.1.1/src/CMakeLists.txt](flightgear-2024.1.1/src/CMakeLists.txt) lists them in an explicit `foreach` order because traditional `ld` requires libraries before their dependents. The modules are functional subsystems: `FDM` (flight dynamics — JSBSim, YASim), `Systems`, `Autopilot`, `Instrumentation`, `Cockpit`, `Aircraft`, `AIModel` (AI traffic), `ATC`, `Scenery`, `Environment`, `Sound`, `Scripting` (Nasal), `Canvas`, `MultiPlayer`, `Network`, `Traffic`, `Time`, `Viewer`, `GUI`, `Input`, `Main`.
**FlightGear 被拆分为约 30 个对链接顺序敏感的模块库。** [flightgear-2024.1.1/src/CMakeLists.txt](flightgear-2024.1.1/src/CMakeLists.txt) 以显式 `foreach` 顺序列出它们，因为传统 `ld` 要求被依赖库在前、依赖者在后。这些模块都是功能子系统：`FDM`（飞行动力学 — JSBSim、YASim）、`Systems`、`Autopilot`、`Instrumentation`、`Cockpit`、`Aircraft`、`AIModel`（AI 交通）、`ATC`、`Scenery`、`Environment`、`Sound`、`Scripting`（Nasal）、`Canvas`、`MultiPlayer`、`Network`、`Traffic`、`Time`、`Viewer`、`GUI`、`Input`、`Main`。

**Three cross-cutting concepts to internalize before editing anything:**
**动手改任何东西之前，先理解这三个贯穿性概念：**

1. **The property tree is the runtime state bus.** SimGear's `SGPropertyNode` (`simgear/props`) is a hierarchical key/value tree. Nearly every subsystem publishes and reads its state through property paths (e.g. `/position/altitude-ft`), and config is loaded into it from XML. Global access lives in `src/Main/fg_props.cxx`. When tracing behavior, follow the property paths, not C++ call chains.
   **属性树是运行时状态总线。** SimGear 的 `SGPropertyNode`（`simgear/props`）是一棵分层的键/值树。几乎每个子系统都通过属性路径（如 `/position/altitude-ft`）发布和读取自身状态，配置也从 XML 加载到树中。全局访问位于 `src/Main/fg_props.cxx`。追踪行为时，要沿着属性路径走，而不是顺着 C++ 调用链。

2. **Nasal is the embedded scripting language.** `simgear/nasal` provides the interpreter; `src/Scripting` hosts the FlightGear bindings. Much of the high-level logic (instruments, autopilot modes, systems) is written in Nasal files under the data package rather than C++. A C++ change is often only half the job — the corresponding Nasal lives in `fgdata` (base package, separate from this source tree).
   **Nasal 是内嵌脚本语言。** `simgear/nasal` 提供解释器；`src/Scripting` 承载 FlightGear 的绑定。大量高层逻辑（仪表、自动驾驶模式、系统）是用数据包下的 Nasal 文件写的，而非 C++。改 C++ 往往只完成了一半 —— 对应的 Nasal 位于 `fgdata`（基础数据包，与本源码树分离）。

3. **Subsystems are managed objects.** `SGSubsystem` (in SimGear) with the `SGSubsystemManager` drives the main-loop update. Subsystems are instantiated and wired together in [src/Main/subsystemFactory.cxx](flightgear-2024.1.1/src/Main/subsystemFactory.cxx); the entry point and bootstrap are in [src/Main/main.cxx](flightgear-2024.1.1/src/Main/main.cxx) and [src/Main/fg_init.cxx](flightgear-2024.1.1/src/Main/fg_init.cxx). To add or reorder a subsystem, register it there.
   **子系统是受管理的对象。** SimGear 中的 `SGSubsystem` 配合 `SGSubsystemManager` 驱动主循环更新。子系统在 [src/Main/subsystemFactory.cxx](flightgear-2024.1.1/src/Main/subsystemFactory.cxx) 中被实例化并装配；入口与引导代码位于 [src/Main/main.cxx](flightgear-2024.1.1/src/Main/main.cxx) 和 [src/Main/fg_init.cxx](flightgear-2024.1.1/src/Main/fg_init.cxx)。要新增或调整子系统顺序，在那里注册。

- C++ sources use `.cxx`/`.hxx`, not `.cpp`/`.hpp`.
  C++ 源文件使用 `.cxx`/`.hxx` 扩展名，而不是 `.cpp`/`.hpp`。
- Third-party code is vendored under each project's `3rdparty/` (e.g. `sqlite3`, `mongoose`, `cjson` in FlightGear; `expat`, `tiny_gltf`, `udns` in SimGear).
  第三方代码内置于各项目的 `3rdparty/` 下（如 FlightGear 中的 `sqlite3`、`mongoose`、`cjson`；SimGear 中的 `expat`、`tiny_gltf`、`udns`）。

## Code comments / 代码注释（用户硬性要求）

**All modified or newly written code MUST carry detailed Chinese comments.** Keep pre-existing English comments as they are; every comment you add or modify must be in Chinese (code identifiers, function names, and technical terms stay in English).
**所有修改或新写的代码必须带详细的中文注释。** 原有英文注释保持不动；新增或修改的注释一律用中文（代码标识符、函数名、技术术语保持英文）。

- **UTF-8 BOM pitfall:** a `.cxx`/`.hxx` file that gains Chinese comments MUST be saved with a UTF-8 BOM, otherwise MSVC decodes the source as GBK and the comment bytes corrupt preprocessor directives (error C1020 意外的 #endif). Add the BOM with: `python -c "..."` writing `\xef\xbb\xbf` prefix.
  **UTF-8 BOM 坑：** 加入中文注释的 `.cxx`/`.hxx` 文件必须保存为带 UTF-8 BOM，否则 MSVC 按 GBK 解码源码，中文注释字节会破坏预处理指令（报 C1020 意外的 #endif）。给文件加 `\xef\xbb\xbf` 前缀即可。
- XML/CMake/Python files are also expected to carry Chinese comments on changed lines.
  XML/CMake/Python 文件修改的部分同样要写中文注释。
