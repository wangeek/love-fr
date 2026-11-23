# love-fr —— LÖVE 11.5 的 FR 帧流捕获分支

> **这是 [LÖVE](https://github.com/love2d/love) 的修改版（fork），不是官方版本。**
> 基于 LÖVE 11.5（tag `11.5`），在 `fr-capture` 分支上加了引擎内的 **FR 帧流捕获与回放**。
> 上游版权与许可（zlib）原样保留，见 [`license.txt`](license.txt)。
>
> `fr-capture` 分支带 LÖVE 截至 `11.5` 的完整历史，FR 提交都落在官方 `11.5` 之后；
> tag `11.5` 就是上游那个发布标签本身，可以直接拿来做 diff。

本仓库只放**引擎侧**改动。流的格式规格、Python 工具族（录制封装 / 判据 / diff / lint）、
文档与测试都在主仓库 **[wangeek/AgentLove2d](https://github.com/wangeek/AgentLove2d)**——
要用 FR 做验证，请从那边入手；本仓库供你自行构建魔改后的 `love.exe`。

---

## 它做什么

给 LÖVE 游戏开一个环境变量，运行时就被逐帧录成一份**文本流**（`.fr`）：每帧记录步长、
输入事件、绘制调用、状态校验和。不需要改游戏代码。

```bash
FR_CAPTURE=1 FR_OUT=run.fr FR_FRAMES=30 FR_DT=0.016 \
  love.exe <你的游戏目录>
```

录出来的流长这样（节选一帧，`…` 处省略了其余绘制行）：

```
frame 3
dt 0.016000
clear 0 0 0 255
draw 1 0 14.000080 20.000000 0.000000 1.000000 1.000000 0.000000 0.000000 255 255 255 255
print 2 "L0" 12.000000 340.000000 0.000000 1.000000 1.000000 255 255 0 255
…
s_crc 4077747399
d_crc 408721732
```

设了 `FR_DT`，两次录制的步长就完全一致；游戏只要自身是确定性的（不读墙钟、随机数有
固定种子），两条流便**逐字节相同**。于是“游戏行为”变成可以用文本工具 diff、grep、
锚行断言的东西——这是给 AI agent 做验证用的：不靠看截图，靠读符号行。

## 相对上游改了什么

基线就是官方 `11.5` 的那个提交（`6eb8d54`）。它的 936 个文件与官方逐一比对 blob 哈希，
全部一致（2026-11-23 核对）。

相对 `11.5`（不含本说明文件，截至 2026-11-23）：**20 个文件，+7813 / −2**，几乎全是新增模块。
现行数字以 `git diff --stat 11.5..fr-capture` 为准。

| 位置 | 内容 |
|---|---|
| `src/modules/graphics/fr/`（新增） | 捕获、编解码、会话写出、状态 diff、资产物化 |
| `src/modules/graphics/wrap_Graphics.cpp` | `+462`：绘制/资产构造的挂钩与 FR 入口 |
| `src/modules/timer/wrap_Timer.cpp` | `love.timer.step` 钩子：记录 dt；`FR_DT` 时覆写返回值 |
| `src/modules/event/`（2 文件，各 +4） | 事件捕获（`Event::pump` / `love.event.push`） |
| `src/love.cpp` | `+17`：退出前冲刷物化缓存、封口会话 |
| `CMakeLists.txt` | `+41`：把 `fr_*` 源文件编进构建 |
| `demo/`（新增） | 捕获验证 rig（`conf.lua` / `kernel.lua` / `main.lua`），也是离线回放的入口 |

对上游**已有代码**只删了 `wrap_Timer.cpp` 里的 2 行：把 `lua_pushnumber(L, instance()->step())`
一行等价展开成多行，并顺带记下 dt。`FR_DT` 未设时，游戏看到的行为不变。

## 环境变量

引擎在 FR 分支里读取的环境变量共 19 个，分三组。不设 `FR_CAPTURE` 时捕获不启用，
不设 `FR_OUT` 时不会写流。

**录制**

| 变量 | 作用 |
|---|---|
| `FR_CAPTURE=1` | 开启捕获：绘制调用不直接执行，改为记录，并在 present 前回放到屏幕 |
| `FR_OUT=<path>` | 流输出路径 |
| `FR_FRAMES=<n>` | 录满 n 帧后封口（进程继续运行） |
| `FR_DT=<f>` | **固定步长**——覆写 `love.timer.step()` 的返回值，两次录制的步长因此一致。只影响返回值：`love.timer.getDelta()` / `getFPS()` 仍按真实时间 |
| `FR_SEED=<n>` | 种子，**只是写进会话头**。引擎不会用它给随机数播种——需要确定性时，游戏得自己读它来播种，并且不读墙钟 |
| `FR_SHA=<64hex>` | 命令表指纹，写进会话头。缺省记全 0 并在 stderr 告警 |
| `FR_MAX_MB=<f>` | 体积上限，达到即封口。判定在帧边界，末帧会完整写完，所以实际大小可能略超 |
| `FR_OVERWRITE=1` | 允许覆盖已存在的 `FR_OUT`。默认拒绝，见下面的“写者护栏” |
| `FR_W` / `FR_H` | 会话头里记的宽高。没有窗口时取不到尺寸，缺省回退 640×480 |
| `FR_LOGLEVEL=full` | 每帧额外写 `st` 行（Lua 堆的状态 diff） |

**离线回放**（`love.exe <本仓库>/demo --offline-replay <流文件>`，把录好的流重新渲染一遍）

| 变量 | 作用 |
|---|---|
| `FR_PLAY=1` | 按流里记录的步长逐帧播放到屏幕 |
| `FR_VIDEO_OUT=<path>` | 回放时把每帧原始像素追加写入该文件 |

**调试 / 实验开关**（一般用不到）

| 变量 | 作用 |
|---|---|
| `FR_BOOT_SCRIPT=<path>` | 捕获态首帧前执行一次的外置 Lua 脚本。典型用途是调 `love.graphics._frStateRoot(根表)`，把 `s_crc` 的观测范围收缩到指定的状态子图 |
| `FR_DELTA=1` | 实验性的增量写出（会话头标 `+d`） |
| `FR_DELTA_I=<n>` | 增量写出时的 I 帧间隔，缺省 32 |
| `FR_NO_REPLAY` | 设了就跳过 present 前的回放（定位崩溃用：区分捕获侧还是回放侧） |
| `FR_NO_SCRC` | 设了就跳过堆状态采集（同样是崩溃定位用） |
| `FR_CRASH_PROBE` | 设了就安装崩溃探针（Windows：SEH 过滤器 + 符号） |

另有一个变量**不是引擎读的**：`FR_HEADLESS`。它由游戏自己的 `conf.lua` 读取并据此设
`t.window = false`（`demo/conf.lua` 是现成例子）。游戏不认识它，它就没有任何效果。

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

Windows 上用 MSVC 时，LÖVE **不能独立构建**：它的 CMakeLists 在非
[megasource](https://github.com/love2d/megasource) 目录布局下会直接报错退出。所以要把本仓库
放进 megasource 里构建。

实际验证过的工具链：VS 2022 BuildTools（cl 19.44）、Windows SDK 10.0.26100、CMake 4.2.1。

1. 取 megasource 并切到 `11.5`。它的默认分支已经是 love 12 / SDL3 的布局（2026-10-27 实测），
   构建 11.5 必须切到这个 tag；该 tag 下依赖库都在仓库树里，不需要初始化子模块。
   ```bat
   git clone https://github.com/love2d/megasource.git megasource
   git -C megasource checkout 11.5
   ```
2. 把本仓库放到 `megasource\libs\love`，停在 `fr-capture` 分支。
3. 配置并构建（PowerShell；`-D` 参数要整体加引号）：
   ```powershell
   cmake -S megasource -B megasource\build -G "Visual Studio 17 2022" "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
   $env:CMAKE_POLICY_VERSION_MINIMUM = '3.5'    # openal 内部还会再调一次 cmake
   cmake --build megasource\build --config Release -- -m
   ```
4. 产物在 `megasource\build\love\Release\`：`love.exe`、`love.dll`，以及 `lua51` / `SDL2` /
   `OpenAL32` / `mpg123` 的 DLL。

`CMAKE_POLICY_VERSION_MINIMUM`：CMake 4.x 不再接受 `cmake_minimum_required(VERSION 3.1)`
这类旧声明，必须给这个变量；4.0 之前的 CMake 不需要它。

## 已验证 / 未验证

**已验证**（都是 Windows x64 + MSVC 2022）：

- 行为：主仓库 180 项测试，覆盖录制确定性（双跑逐字节）、离线回放、事件重演、写者护栏等
- 真实游戏：`newgame`《剑侠无敌》零改动录制（30 帧冒烟）；另一个 LÖVE 重制工程的整套冒烟
  判据（约 40 项）在用它当主验证手段
- 构建：上面那组命令于 2026-10-27 在 11.5 基线源码上**从零全量**跑通过一次（约 9 分钟）。
  之后的 FR 改动（含当前提交）都是用**增量构建**验证的

**未验证**：

- **「FR 分支 + 从零全量构建」这个组合没有单独重跑过**——增量构建用的是工作区文件，
  这些文件已确认与提交内容一致，但完整的干净克隆构建尚未实测
- Windows 之外的任何平台（写者护栏的独占锁依赖 Windows API）
- macOS / iOS / Android / Linux 构建
- 除上述两个游戏之外的兼容性。捕获态下部分 API 走的是“语义替代”而非真执行，
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
