/**
 * FR 堆 CRC 档（B6 Task 1，fork 魔改，手写层）。
 *
 * 职责：present 时刻（帧 commit 点）对 LuaJIT 全堆 GCtab 做确定性序 CRC
 * ——end 段 `s_crc <u32>` 行的数据源（docs/fr-spec.md §4/§7；拍板口径见
 * docs/plans/2026-10-27-b6-statediff.md）。
 *
 * 确定性序（拍板）：沿 G(L)->gc.root 的 nextgc 链（分配序的逆——无 GC
 * 紧缩下稳定）× 槽位序（数组部 asize 个 TValue 后 hash 部 hmask+1 个
 * Node 的 val TValue，Node 数组线性）。nil 槽也计入（表大小天然进 CRC）；
 * Node.key 不计（只 val）。GC 紧缩改链序 → CRC 变 = 已知边界（跨紧缩的
 * 流间对比不承诺）。
 *
 * GC 载荷规范化（本实现定形，如实登记）：str/tab/udata/func 等 GC 引用槽
 * 与 lightuserdata 槽只计 tag 位（GC64 NaN 框高 17 位 / 非 GC64 的 it
 * 域），载荷指针位清零——堆地址随 ASLR 每进程漂移，原样计入则同参双跑
 * 不可复现；number 槽计全 8 字节位型（double/零扩 int，确定性）；nil/
 * true/false 计原位型。结构敏感性：每表 asize/hmask 头部混入 + 对象计数
 * 收尾混入（值同结构变仍改 CRC）。
 *
 * 只读红线：不置 marked、不调任何 lj_ 内部函数、不触发 GC——纯字节读；
 * 收集点在 C 钩/显式出口内（无 Lua 执行窗口），GC 不会推进。
 *
 * 收集口径：w_present 的 capture 分支常开调 capture_state_crc（无窗也采
 * ——tee 无头/有窗逐字节一致锚；回放才是 GL 依赖）；验证帧（demo 不走
 * present）与 --sim 独立循环由 demo 侧显式调 _frStateCRC——两路径同一
 * C 遍历函数。每帧缓存一份（_frTakeSCrcs 全量取走，tee 组装 end 段）。
 **/

#pragma once

// LOVE
#include "common/config.h"
#include "common/int.h"

#include <vector>

struct lua_State;

namespace fr
{

// 全堆 GCtab 确定性序 CRC（纯计算，不入帧缓存；红线见文件头）。
love::uint32 fr_state_crc(struct lua_State *L);

// present 钩入口：计算并压入每帧 s_crc 缓存（开关由调用方判，常开档）。
void capture_state_crc(struct lua_State *L);

// Lua 出口：
//   love.graphics._frStateCRC()  → 计算堆 CRC 入当帧缓存并返回 u32
//       （验证帧 / --sim 循环的显式收集口——与 present 钩同一遍历）；
//   love.graphics._frTakeSCrcs() → 每帧 s_crc 数组（收集序 = 帧序）。
int w_frStateCRC(lua_State *L);
int w_frTakeSCrcs(lua_State *L);

} // fr
