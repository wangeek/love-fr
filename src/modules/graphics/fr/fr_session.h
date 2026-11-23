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
 * env（均为可选；未设 FR_OUT 时整个写出器不启用、零开销）：
 *   FR_OUT   流路径。**写者护栏（三条，见 open_stream_file 注释）**：
 *            ① 目标已存在 → 默认拒绝覆盖并停用写出（FR_OVERWRITE=1 放行）；
 *            ② 独占打开（Windows _fsopen+_SH_DENYRW）——同路径第二个写者
 *               拿共享冲突，明确报错而不是互相踩；
 *            ③ FR_MAX_MB 体积封口（帧边界判，末帧完整；支持小数）。
 *   FR_SHA   64 hex 表指纹。缺省/非法 → 会话头记全 0 + stderr 告警
 *            （不停用；判据对账需真指纹时由调用方显式给）。
 *   FR_SEED  种子，写会话头（缺省 0）。
 *   FR_FRAMES 帧数上限，缺省无限；到限封口不杀进程。
 *   FR_DT    **固定步长——覆写 love.timer.step 的返回值**（游戏循环按它走，
 *            是双跑逐字节的前提）。未设时记录真实 dt。
 *   FR_W/FR_H 会话头几何；无头（t.window=false）时 getWidth() 返 0，
 *            缺省回退 640×480（否则落盘 frb1 0 0，流不可解析）。
 *   FR_LOGLEVEL=full → 写 st 行；FR_DELTA=1 → 增量编码（head 标 +d）。
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
