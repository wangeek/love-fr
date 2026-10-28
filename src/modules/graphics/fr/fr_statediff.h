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
 * 按常数 0xC1C1E1E1 计并记 err 行一次（demo 无环，防御性）；str 槽 =
 * tag 位 + (len+内容) CRC32（B7a 单一化：与 diff 槽指纹同规则，字符串
 * 值变化进 s_crc）、udata/func 等其余引用槽只计 tag 位、number 全位型、
 * nil/bool 原位型（canonicalization 与全堆口径同一函数）。递归深度不设限
 * （demo 状态浅 2-3 层，深图栈风险如实登记）。
 *
 * 全堆口径（Task 1，回退路径）：沿 G(L)->gc.root 的 nextgc 链（分配序的
 * 逆——无 GC 紧缩下稳定）× 槽位序（数组部 asize 个 TValue 后 hash 部
 * hmask+1 个 Node 的 val TValue，Node 数组线性）。nil 槽也计入（表大小
 * 天然进 CRC）；Node.key 不计（只 val）。GC 紧缩改链序 → CRC 变 = 已知
 * 边界（跨紧缩的流间对比不承诺）。
 *
 * GC 载荷规范化（两口径共用，本实现定形，如实登记）：str 槽 = tag 位 +
 * (len+内容) CRC32（B7a 单一化，内容跨进程确定——CRC 增量直读 strdata
 * 字节不复制）；tab/udata/func 等 GC 引用槽与 lightuserdata 槽只计
 * tag 位（GC64 NaN 框高 17 位 / 非 GC64 的 it 域），载荷指针位清零——
 * 堆地址随 ASLR 每进程漂移，原样计入则同参双跑不可复现；number 槽计
 * 全 8 字节位型（double/零扩 int，确定性）；nil/true/false 计原位型。
 * 结构敏感性：每表 asize/hmask 头部混入 + 对象计数收尾混入（值同结构
 * 变仍改 CRC）。
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
 *
 * diff 档（Task 2，FR_LOGLEVEL=full）：同根子图的影子镜像 diff → 当帧
 * log 段 st 物理行（`st <objid> "<key>" <variant>`，cmd.tbl st 签名：
 * obj u32 / key str / val variant 两 token）+ err 行（cmd.tbl err what str
 * 单串——表重建 `err "st-rehash <id> <asize> <hmask>"`（err notes"表重建
 * 计数"语义位，计划口径 *rehash* 行按 cmd.tbl 为准收进 err 载体）与值域
 * 外槽 `err "st-unsupported <id> <slot> <tag>"`）。镜像 = statediff 自有
 * 内存（堆外，不算写 VM）：数组部下标对位存 canonical 槽指纹、hash 部按
 * key canonical 串寻址（Node 落桶随 Brent 搬移漂移，键寻址才稳）；表重建
 *（首见/asize|hmask 变）全槽重发；nil 槽不产行（variant 域无 nil，
 * cmd.tbl 为准——变 nil=槽缺席，值保真由 s_crc 全位型承担）。对象 id 跨
 * 帧单调递增不复用；Table* 复用弱校验 (asize,hmask,指纹) + id 换新
 *（epoch 最简实现，demo 深度不触发，防御登记）。行序确定性：根表槽行在
 * 前、子表递归在后，槽序 = 数组部 0 基下标 + hash 部 key 串字节序。
 **/

#pragma once

// LOVE
#include "common/config.h"
#include "common/int.h"

#include <string>
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

// ------------------------------------------------------------------
// B6 Task 2：diff 档（影子镜像 + st 物理行 + 表重建，FR_LOGLEVEL=full）
// ------------------------------------------------------------------

// FR_LOGLEVEL=full 开档（env 首读缓存，与 capture_enabled 同款纪律）。
bool state_diff_full();

// 当帧 diff 行收集（根取回与 sub CRC 同根；st/err 行序确定性：子图先根表
// 自身槽行、后子表递归（槽序 = 数组部 0 基下标序 + hash 部 key canonical
// 串字节序——Node 线性序跨进程不可复现，与 CRC multiset 同因））。未注册
// 根 → 无行（全堆口径不产 diff 行，stderr 登记）；纯读不写 VM，红线同上。
std::vector<std::string> fr_state_diff_rows(struct lua_State *L);

// present 钩入口：full 级收集 diff 行入当帧缓存（非 full 自 no-op）。
void capture_state_diff(struct lua_State *L);

// Lua 出口：
//   love.graphics._frStateRoot(t)    → 登记状态根（table 后者覆盖 / nil 清除）；
//   love.graphics._frStateSubCRC()   → 子图 CRC 入当帧缓存并返回 u32
//       （验证帧 / --sim 循环的显式收集口——与 present 钩同一遍历；
//       未注册根回退全堆，与 present 钩口径一致）；
//   love.graphics._frStateCRC()      → 全堆 CRC 入当帧缓存并返回 u32
//       （Task 1 口径保留，审计/对照用——demo 收集点不走此口）；
//   love.graphics._frTakeSCrcs()     → 每帧 s_crc 数组（收集序 = 帧序）；
//   love.graphics._frStateDiffRows() → 当帧 diff 行（st/err）入缓存并返回
//       行表（验证帧 / --sim 的 diff 档显式收集口——与 present 钩同一遍历；
//       档位策略归调用点，本出口是机制口）；
//   love.graphics._frTakeSDiffRows() → 每帧 diff 行数组的数组（帧序，tee
//       组装 log 段用；收集序与 _frTakeSCrcs 同帧纪律——每帧恰一份）。
int w_frStateRoot(lua_State *L);
int w_frStateSubCRC(lua_State *L);
int w_frStateCRC(lua_State *L);
int w_frTakeSCrcs(lua_State *L);
int w_frStateDiffRows(lua_State *L);
int w_frTakeSDiffRows(lua_State *L);

// B10：writer 只读视图（fr_session 冲帧用；游标归 writer 自持——收集点
// present 钩每帧恰一份，与收集序同帧纪律）。
const std::vector<love::uint32> &fr_view_scrcs();
const std::vector<std::vector<std::string>> &fr_view_sdiffs();

} // fr
