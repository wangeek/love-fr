# love-fr —— LÖVE 11.5 的 FR 帧流捕获分支

> **这是 [LÖVE](https://github.com/love2d/love) 的修改版（fork），不是官方版本。**
> 基于 LÖVE 11.5（tag `11.5`），在 `fr-capture` 分支上加了引擎内的 **FR 帧流捕获与回放**。
> 上游版权与许可（zlib）原样保留，见 [`license.txt`](license.txt)。

本仓库只放**引擎侧**改动。流的格式规格、Python 工具族（录制封装 / 判据 / diff / lint）、
文档与测试都在主仓库 **[wangeek/AgentLove2d](https://github.com/wangeek/AgentLove2d)**——
要用 FR 做验证，请从那边入手；本仓库供你自行构建魔改后的 `love.exe`。

---

## 它做什么

给任意 LÖVE 游戏开一个环境变量，游戏在运行时就被逐帧录成一份**文本流**（`.fr`）：
每帧记录步长、输入事件、绘制调用、状态校验和。零游戏侧改动。

```bash
FR_CAPTURE=1 FR_OUT=run.fr FR_FRAMES=30 FR_DT=0.016 FR_SEED=1 \
  love.exe <你的游戏目录>
```

录出来的流长这样（每帧一个块）：

```
frame 3
dt 0.016000
clear 0 0 0 255
draw 1 0 14.000080 20.000000 0.000000 1.000000 1.000000 0.000000 0.000000 255 255 255 255
s_crc 4077747399
d_crc 408721732
```

固定 `FR_DT` + `FR_SEED` 后两次录制**逐字节相同**，于是“游戏行为”变成可以用文本工具
diff、grep、锚行断言的东西——这是给 AI agent 做验证用的：不靠看截图，靠读符号行。

## 相对上游改了什么

相对 `11.5`：**20 个文件，+7802 / −2**。对 LÖVE 本体的侵入极小，改动几乎全是新增模块。

| 位置 | 内容 |
|---|---|
| `src/modules/graphics/fr/`（新增） | 捕获、编解码、会话写出、状态 diff、资产物化 |
| `src/modules/graphics/wrap_Graphics.cpp` | `+462`：绘制/资产构造的挂钩与 FR 入口 |
| `src/modules/timer/wrap_Timer.cpp` | `love.timer.step` 钩子：记录 dt；`FR_DT` 时覆写返回值 |
| `src/modules/event/`（2 文件，各 +4） | 事件捕获（`Event::pump` / `love.event.push`） |
| `src/love.cpp` | `+17`：退出前封口会话、离线回放/`--sim` 模式入口 |
| `CMakeLists.txt`、`.gitignore` | 把 `fr_*` 编进构建 |
| `demo/`（新增） | 捕获验证 rig（`conf.lua` / `kernel.lua` / `main.lua`） |

对上游**已有代码**的净删除只有 2 行（都在 `wrap_Timer.cpp`）。

## 环境变量

全部可选。不设 `FR_OUT` 时整个写出器不启用。

| 变量 | 作用 |
|---|---|
| `FR_CAPTURE=1` | 开启捕获：绘制调用不直接执行，改为记录后在 present 时回放 |
| `FR_OUT=<path>` | 流输出路径。设置后才真正写流 |
| `FR_FRAMES=<n>` | 录 n 帧后封口（进程继续跑） |
| `FR_DT=<f>` | **固定步长**——覆写 `love.timer.step` 的返回值。确定性的前提。注意只改返回值：`love.timer.getDelta()` / `getFPS()` 仍基于真实时间 |
| `FR_SEED=<n>` | 种子，写进会话头 |
| `FR_SHA=<64hex>` | 命令表指纹，写进会话头。缺省记全 0 并告警 |
| `FR_MAX_MB=<f>` | 体积上限，达到即封口（在帧边界判断，所以末帧会完整写完，实际大小可略超） |
| `FR_OVERWRITE=1` | 允许覆盖已存在的 `FR_OUT` |
| `FR_HEADLESS=1` | 无头。**引擎本身不读这个变量**——由游戏的 `conf.lua` 读它并设 `t.window = false`（见 `demo/conf.lua`）。第三方游戏若不认识它，此变量无效果 |
| `FR_W` / `FR_H` | 会话头几何。无头时窗口尺寸为 0，缺省回退 640×480 |
| `FR_LOGLEVEL=full` | 额外写 `st` 行（堆状态 diff） |
| `FR_VIDEO_OUT=<path>` | 逐帧原始像素另存（供转视频） |

### 写者护栏

录制出过一次事故：两个进程写同一个 `FR_OUT`，后启动的把前一个正在写的流截断了，
结果产出一条 20.4GB 的坏流。之后 C 侧写出器加了三条护栏：

1. **目标已存在 → 默认拒绝覆盖**，并在 stderr 说明原因；要覆盖须显式 `FR_OVERWRITE=1`。
2. **独占打开**（Windows `_fsopen` + `_SH_DENYRW`）：同一路径的第二个写者会拿到共享冲突错误。
   非 Windows 平台退化为只有第 1 条。
3. **`FR_MAX_MB` 体积封口**。

**范围限制**：护栏只覆盖 C 侧写出器（`FR_OUT`）。`demo/main.lua --out` 那条 Lua 侧的
tee 路径用的是普通 `io.open(..., "wb")`，**没有**这些护栏。

## 构建

本仓库只是 `love` 这一层；依赖库在 [megasource](https://github.com/love2d/megasource) 里。
Windows + Visual Studio 的做法：

1. 取 megasource（11.5 对应版本），把本仓库放到 `megasource/libs/love`。
2. 配置并构建：
   ```bat
   set CMAKE_POLICY_VERSION_MINIMUM=3.5
   cmake -G "Visual Studio 17 2022" -A x64 -S megasource -B megasource\build
   cmake --build megasource\build --config Release
   ```
3. 产物在 `megasource\build\love\Release\love.exe`。

`CMAKE_POLICY_VERSION_MINIMUM` 是 openal 子项目所需的。其他平台未做验证（见下）。

## 已验证 / 未验证

**已验证**（Windows x64，MSVC 2022）：

- 主仓库 180 项测试，覆盖录制确定性（双跑逐字节）、离线回放、事件重演、写者护栏等
- 第三方 LÖVE 游戏零改动录制（`newgame`《剑侠无敌》30 帧冒烟；另一个 LÖVE 重制工程
  的 39 项冒烟判据在用它当主验证手段）

**未验证**：

- Windows 之外的任何平台（护栏的独占锁依赖 Windows API）
- macOS / iOS / Android / Linux 构建
- 除上述两个游戏之外的游戏兼容性。捕获态下部分 API 走的是“语义替代”而非真执行，
  shader、部分 canvas 操作等属于已知边界——细节见主仓库 `docs/love2d-gap.md`

**性能特征**：捕获态下每帧的回放成本随会话行数增长，长会话有增量重放缓解，
但不是为“录几小时”设计的。请始终设置 `FR_FRAMES` 或 `FR_MAX_MB`。

## 目录

```
src/modules/graphics/fr/
  fr_capture.cpp/.h      捕获点、资产登记与代理、回放执行、离线回放
  fr_session.cpp/.h      流文件写出器（帧块组装、护栏）
  fr_statediff.cpp/.h    堆状态 diff（st 行）与 s_crc
  fr_codec.c/.h          行级编解码（由命令表生成，勿手改）
  fr_materialize.cpp/.h  登记字节 → 真 Image/Font 物化
demo/                    捕获验证 rig
```

`fr_codec.*` 是生成物：真源是主仓库的 `tables/cmd.tbl`，改表后重新生成，不要手改。

## 许可

上游 LÖVE 为 zlib 许可，本仓库的修改部分同样以 zlib 许可发布。
上游及第三方组件的完整许可文本见 [`license.txt`](license.txt)。

---

以下为上游原 README，原样保留。

---

LÖVE is an *awesome* framework you can use to make 2D games in Lua. It's free, open-source, and works on Windows, macOS, Linux, Android, and iOS.

[![Build Status: Windows](https://ci.appveyor.com/api/projects/status/chc0hdr08wv1d5c7?svg=true)](https://ci.appveyor.com/project/slime73/love)
[![Build Status: Github CI](https://github.com/love2d/love/workflows/continuous-integration/badge.svg)](https://github.com/love2d/love/actions?query=workflow%3Acontinuous-integration)

Documentation
-------------

We use our [wiki][wiki] for documentation.
If you need further help, feel free to ask on our [forums][forums], our [Discord server][discord], or our [subreddit][subreddit].

Repository
----------

We use the 'main' branch for patch development of the current major release, and therefore it should not be considered stable.
There may also be a branch for the next major version in development, which is named after that version.

We tag all our releases (since we started using mercurial and git), and have binary downloads available for them.

Experimental changes are developed in a separate [love-experiments][love-experiments] repository.

Builds
------

Files for releases are in the [releases][releases] section on GitHub. [The site][site] has links to files and additional platform content for the latest release.

There are also unstable/nightly builds:

- Builds for some platforms are automatically created after each commit and are available through GitHub's CI interfaces.
- For ubuntu linux they are in [ppa:bartbes/love-unstable][unstableppa]
- For arch linux there's [love-git][aur] in the AUR.

Contributing
------------

The best places to contribute are through the issue tracker and the official Discord server or IRC channel.

For code contributions, pull requests and patches are welcome. Be sure to read the [source code style guide][codestyle].
Changes and new features typically get discussed in the issue tracker or on Discord or the forums before a pull request is made.

Compilation
-----------

### Windows
Follow the instructions at the [megasource][megasource] repository page.

### *nix
Run `platform/unix/automagic` from the repository root, then run ./configure and make.

	$ platform/unix/automagic
	$ ./configure
	$ make

When using a source release, automagic has already been run, and the first step can be skipped.

### macOS
Download or clone [this repository][dependencies-apple] and copy, move, or symlink the `macOS/Frameworks` subfolder into love's `platform/xcode/macosx` folder.

Then use the Xcode project found at `platform/xcode/love.xcodeproj` to build the `love-macosx` target.

### iOS
Building for iOS requires macOS and Xcode.

#### LÖVE 11.4 and newer
Download the `love-apple-dependencies` zip file corresponding to the LÖVE version being used from the [Releases page][dependencies-ios],
unzip it, and place the `iOS/libraries` subfolder into love's `platform/xcode/ios` folder.

Or, download or clone [this repository][dependencies-apple] and copy, move, or symlink the `iOS/libraries` subfolder into love's `platform/xcode/ios` folder.

Then use the Xcode project found at `platform/xcode/love.xcodeproj` to build the `love-ios` target.

See `readme-iOS.rtf` for more information.

#### LÖVE 11.3 and older
Download the `ios-libraries` zip file corresponding to the LÖVE version being used from the [Releases page][dependencies-ios],
unzip it, and place the `include` and `libraries` subfolders into love's `platform/xcode/ios` folder.

Then use the Xcode project found at `platform/xcode/love.xcodeproj` to build the `love-ios` target.

See `readme-iOS.rtf` for more information.

### Android
Visit the [Android build repository][android-repository] for build instructions.

Dependencies
------------

- SDL2
- OpenGL 2.1+ / OpenGL ES 2+
- OpenAL
- Lua / LuaJIT / LLVM-lua
- FreeType
- ModPlug
- mpg123
- Vorbisfile
- Theora

[site]: https://love2d.org
[wiki]: https://love2d.org/wiki
[forums]: https://love2d.org/forums
[discord]: https://discord.gg/rhUets9
[subreddit]: https://www.reddit.com/r/love2d
[dependencies-apple]: https://github.com/love2d/love-apple-dependencies
[dependencies-ios]: https://github.com/love2d/love/releases
[megasource]: https://github.com/love2d/megasource
[unstableppa]: https://launchpad.net/~bartbes/+archive/love-unstable
[aur]: https://aur.archlinux.org/packages/love-git
[love-experiments]: https://github.com/slime73/love-experiments
[codestyle]: https://love2d.org/wiki/Code_Style
[android-repository]: https://github.com/love2d/love-android
[releases]: https://github.com/love2d/love/releases
