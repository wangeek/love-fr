/**
 * FR 状态 CRC 档（B6 Task 1/1b，fork 魔改，手写层）。
 *
 * 职责：present 时刻（帧 commit 点）对 LuaJIT 状态做确定性 CRC——end 段
 * `s_crc <u32>` 行的数据源（docs/fr-spec.md §4/§7；拍板口径见
 * docs/plans/2026-10-27-b6-statediff.md）。
 *
 * 口径（Task 1b 裁定，D8"内核 VM 唯一进堆镜像范围"）：s_crc = 注册状态根
 * 可达子图的 CRC。根由 Lua 侧登记（demo 注册 kernel.state）——录制/重演
 * 两进程的 args 表/簿记表形态天然不同（Task 1 实证），全堆 CRC 跨进程等值
 * 结构性不可达；子图只含内核状态（同代码+同事件序列构建），天然可等值。
 * 未注册根 → 回退全堆口径（返回值无标记，从简裁定；demo 恒注册）。
 *
 * 子图序：从根表递归记忆化——tableCRC(t) = asize/hmask 头部 + 数组部序列
 * 贡献（保序）+ hash 部 multiset 贡献（每槽独立 digest 求和，加法交换律
 * ——字符串哈希种子每进程随机，槽位线性序跨进程不可复现，与全堆口径同理）；
 * table 引用槽 = canonical tag 位 + 子表递归 CRC（记忆化 map<Table*,u32>，
 * 同表多处引用只算一次；类型可辨与内容敏感两全）；环（in-progress 引用）
 * 按常数 0xC1C1E1E1 计并记 err 行一次（demo 无环，防御性）；str/udata/func
 * 槽只计 tag 位、number 全位型、nil/bool 原位型（canonicalization 与全堆
 * 口径同一函数）。递归深度不设限（demo 状态浅 2-3 层，深图栈风险如实登记）。
 *
 * 全堆口径（Task 1，回退路径）：沿 G(L)->gc.root 的 nextgc 链（分配序的
 * 逆——无 GC 紧缩下稳定）× 槽位序（数组部 asize 个 TValue 后 hash 部
 * hmask+1 个 Node 的 val TValue，Node 数组线性）。nil 槽也计入（表大小
 * 天然进 CRC）；Node.key 不计（只 val）。GC 紧缩改链序 → CRC 变 = 已知
 * 边界（跨紧缩的流间对比不承诺）。
 *
 * GC 载荷规范化（两口径共用，本实现定形，如实登记）：str/tab/udata/func
 * 等 GC 引用槽与 lightuserdata 槽只计 tag 位（GC64 NaN 框高 17 位 / 非
 * GC64 的 it 域），载荷指针位清零——堆地址随 ASLR 每进程漂移，原样计入
 * 则同参双跑不可复现；number 槽计全 8 字节位型（double/零扩 int，确定性）；
 * nil/true/false 计原位型。结构敏感性：每表 asize/hmask 头部混入 + 对象
 * 计数收尾混入（值同结构变仍改 CRC）。
 *
 * 只读红线（两口径同）：不置 marked、不调任何 lj_ 内部函数、不触发 GC
 * ——纯字节读；收集点在 C 钩/显式出口内（无 Lua 执行窗口），子图根取回
 * 用公共 API（lightuserdata key rawget registry，无分配无 GC 步进）。
 * 根登记（fr_state_root_set）在 Lua 执行窗口内完成（rawset registry）；
 * root 表被内核持有，Registry 强引用不改变 demo 生命周期语义。
 *
 * 收集口径：w_present 的 capture 分支常开调 capture_state_crc（根已注册
 * →子图 CRC，否则全堆——无窗也采，tee 无头/有窗逐字节一致锚；回放才是
 * GL 依赖）；验证帧（demo 不走 present）与 --sim 独立循环由 demo 侧显式
 * 调 _frStateSubCRC——两路径同一 C 遍历。每帧缓存一份（_frTakeSCrcs
 * 全量取走，tee 组装 end 段）。
 **/

#pragma once

// LOVE
#include "common/config.h"
#include "common/int.h"

#include <vector>

struct lua_State;

namespace fr
{

// 全堆 GCtab 确定性序 CRC（纯计算，不入帧缓存；红线见文件头）——Task 1
// 口径，兼作未注册根时的回退路径。
love::uint32 fr_state_crc(struct lua_State *L);

// 状态根登记：idx 处 table 登记为子图根（Registry 存 ref，多次调用后者
// 覆盖；nil 清除）。
void fr_state_root_set(struct lua_State *L, int idx);

// 注册状态根可达子图 CRC（递归记忆化，序见文件头）。未注册/非表根 →
// 回退全堆 fr_state_crc（返回值无标记，从简裁定）。
love::uint32 fr_state_sub_crc(struct lua_State *L);

// present 钩入口：计算并压入每帧 s_crc 缓存（开关由调用方判，常开档；
// 根已注册 → 子图 CRC，否则全堆）。
void capture_state_crc(struct lua_State *L);

// Lua 出口：
//   love.graphics._frStateRoot(t)    → 登记状态根（table 后者覆盖 / nil 清除）；
//   love.graphics._frStateSubCRC()   → 子图 CRC 入当帧缓存并返回 u32
//       （验证帧 / --sim 循环的显式收集口——与 present 钩同一遍历；
//       未注册根回退全堆，与 present 钩口径一致）；
//   love.graphics._frStateCRC()      → 全堆 CRC 入当帧缓存并返回 u32
//       （Task 1 口径保留，审计/对照用——demo 收集点不走此口）；
//   love.graphics._frTakeSCrcs()     → 每帧 s_crc 数组（收集序 = 帧序）。
int w_frStateRoot(lua_State *L);
int w_frStateSubCRC(lua_State *L);
int w_frStateCRC(lua_State *L);
int w_frTakeSCrcs(lua_State *L);

} // fr
