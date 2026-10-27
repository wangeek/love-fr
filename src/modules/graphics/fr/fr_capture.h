/**
 * FR 捕获基础设施（B4-2 fork 魔改，手写层）。
 *
 * 职责：FR_CAPTURE=1 时，把 wrap_Graphics.cpp w_* 边界解析好的调用参数
 * 记录为 .fr 行文本（与 L0 产流样机同构，文本判据链已全通），present 前
 * 由 capture_replay 遍历行缓冲调 Graphics 的 C++ 接口执行——屏幕显示的是
 * 回放结果（视觉真源 = IR，docs/decisions.md D8）。
 *
 * 与 fr_codec（gen_c 生成物）的边界：本任务捕获层直接产行文本，不走
 * fr_codec 的 C API——二进制 .frb 编码/tee 在 B4 后续任务接入；届时行文本
 * 仍是判据真源，fr_codec 只承担字节编解码。勿在本层手写编解码。
 *
 * 行格式：`clear r g b a`，rgba8（0-255 整数，cmd.tbl:32，sealed）。
 * 记录用 lroundf(f*255)（toColor32 是截断，1/255 会被截成 0，不可用）。
 *
 * 本批边界（Task 2）：
 *   - 只捕获 clear；draw/print/rect 等归 Task 3；
 *   - 多渲染目标表形式（colors 数组）取第一个有值目标；无颜色 clear
 *     （clear(false)/纯 stencil/depth）不产行；
 *   - stencil/depth 不在 .fr clear 行格式内，回放按 w_clear 缺省
 *     （stencil=0、depth=1.0）执行；
 *   - 行缓冲 = 全会话累积，不消费（tee₁ 于会话末取全量；clear 回放幂等）。
 **/

#pragma once

// LOVE
#include "common/config.h"
#include "modules/graphics/Graphics.h"

#include <string>
#include <vector>

struct lua_State;

namespace fr
{

// FR_CAPTURE=1 开启；env 只在首次调用读一次并缓存（启动口径）。
bool capture_enabled();

// w_clear 挂钩记录点：真执行的抑制由调用方（wrap_Graphics.cpp）提前 return
// 负责，本函数只记录。颜色分量域 0-1 浮点 → rgba8 整数。
void capture_clear(const love::graphics::OptionalColorf &color,
                   const std::vector<love::graphics::OptionalColorf> &colors);

// 回放：遍历行缓冲，逐行调 Graphics 的 C++ 接口（与 w_* 同一 instance()
// 方法，绕过 Lua 边界）。不消费缓冲。缓冲空 = no-op（开关关时安全可调）。
void capture_replay(love::graphics::Graphics *gfx);

// 全部行文本副本（供 Lua 侧会话组装 / tee₁）。
std::vector<std::string> capture_take_rows();

// Lua 出口：love.graphics._frTakeSession() → 行文本数组；
// love.graphics._frReplay() → 对当前 Graphics 实例回放一次。
int w_frTakeSession(lua_State *L);
int w_frReplay(lua_State *L);

} // fr
