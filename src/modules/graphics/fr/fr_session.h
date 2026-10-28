/**
 * FR C 侧会话写出器（B10）——任意游戏零改动出流的最后一环。
 *
 * 触发：FR_OUT=<path>（FR_CAPTURE=1 下生效）。会话组装从 demo rig 的 Lua
 * （main.lua write_session）下沉到 fork C 层：present 钩 = 帧边界，逐帧
 * 追加帧块；love.cpp 在 lua_close 前调 fr_session_finish 收尾（GL 与模块
 * 仍活，与物化缓存冲刷同点）。
 *
 * 帧块行序（fr-spec 封版，demo 组装同构）：
 *   frame <n> / dt <f32> / [asset 行（仅 frame 0，checkpoint 注入）] /
 *   ev 行（in 段，C 侧 pump/push 捕获）/ err 行（log 段，任何档）/
 *   st 行（log 段，仅 FR_LOGLEVEL=full）/ out 行 / s_crc / d_crc
 *
 * env：FR_OUT（路径，wb 二进制写防 CRT 文本模式 CRLF 改写）、FR_SHA
 *（64 hex 表指纹，缺/坏 → 禁用+stderr）、FR_SEED（缺省 0）、
 * FR_FRAMES（帧数上限，缺省无限；到限封口不杀进程）、FR_DT（dt 兜底，
 * 缺省 0.016667——游戏未调 timer.step 时用，一次性 stderr 告警）。
 *
 * 保真度注记：第三方游戏未注册状态根 → s_crc 走全堆回退（同进程指纹
 * 有效，跨进程 sim 重演等值结构性不可达——B6 实证）；第三方 v1 判据
 * 主力 = 拓扑 B 离线回放 D_CRC/像素锚，s_crc 照记降为参考。
 *
 * 与 demo --out（Lua tee）并存正交：都设则两份（调用方自择）。
 **/

#pragma once

// LOVE
#include "common/config.h"

namespace love
{
namespace graphics
{
class Graphics;
}
}

namespace fr
{

// present 钩逐帧调用（capture_state_crc/diff 之后——当帧 s_crc/st 已入
// 缓冲）。FR_OUT 未设时首调自禁用（零开销路径）。
LOVE_EXPORT void fr_session_frame(love::graphics::Graphics *gfx);

// 收尾（lua_close 前 / FR_FRAMES 到限）：冲缓冲 + 关文件。幂等。
LOVE_EXPORT void fr_session_finish();

} // fr
