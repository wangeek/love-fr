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
 * Task 3 扩展行（cmd.tbl:33-35，sealed）：
 *   draw <tex> <quad> x y rot sx sy ox oy r g b a   （浮点 %.6f）
 *   print <font> "text" x y rot sx sy r g b a        （text 走 fr.esc 同构转义）
 *   rect <mode> x y w h rx ry width r g b a           （mode: 0=fill 1=line）
 * 资产行（cmd.tbl asset）进独立缓冲 fr_take_asset_rows（L0 asset_rows 同构，
 * frame 0 = checkpoint 注入，会话组装时机归 Task 4）。
 *
 * 语义替代四件套（Task 3）：FR_CAPTURE=1 时 w_newImage/newFont/newCanvas/
 * newQuad 提前转入 w_frNew*——不建真 GPU 资产，登记 CPU 字节
 * （B4 阶段三真字节口径：newImage(ImageData) 路径登记 ImageData 像素 RGBA8
 * hex；newFont(size) 登记内嵌 Vera.ttf 字节 + size；文件路径读真实字节）
 * 后返回资产代理 userdata（full userdata + luaL_newmetatable("fr_asset_proxy")
 * 元表，携带 id/kind；弃 lightuserdata 方案：代理要过 GC 与元表类型检查，
 * full userdata 与 11.5 wrap 惯例同构）。
 * Canvas 边界：回读自证 rig 需要真 Canvas（Canvas:newImageData 像素断言），
 * demo 的验证帧走 _frNewCanvasReal（开关无关真建，dpiScale=1 定像素域）；
 * 语义替代版 newCanvas 只服务"内核代码里当资产引用"场景（登记+代理）。
 *
 * 回放渲染（B4 阶段三真物化口径）：draw/print 行回放优先走真纹理物化
 * （fr_materialize：登记字节 → Image/Font，会话内缓存）；未登记 id 或物化
 * 失败回退占位——draw = 伪色矩形（伪色按 id 确定性映射：1→白、2→青、
 * 其它→灰）+ stderr 告警，print = 同伪色小矩形（8x16 每字符）；
 * rect 行回放 = 真实 rectangle（无资产依赖）。
 *
 * 本批边界（Task 3）：
 *   - draw 的 rot/sx/sy/ox/oy 原样记行（demo 恒 0/1，L0 shim 同口径——结算
 *     只动 x/y，锚定 Texture::draw 的 Matrix4(tm, m) 复合 + transformXY 两点法）；
 *     物化回放侧由裸参数重建 Matrix4(x,y,rot,sx,sy,ox,oy,0,0)
 *     （luax_checkstandardtransform 同构，kx/ky 恒 0 不记行）；
 *   - print 多段 ColoredString 顺序拼接为单串（demo 单段）；回放行解析
 *     手写引号扫描 + 转义反转义（esc 的逆，tee round-trip 覆盖）；
 *   - rect 行 x/y 经 getTransform 结算，w/h 直通（L0 shim 同口径 TODO）；
 *     rx/ry>0 的圆角回放降级直角（demo 不传）；
 *   - 回放执行时变换栈取 present 时刻状态（demo 恒 origin，捕获即结算后
 *     直通坐标，无二次变换问题）；
 *   - 行缓冲 = 全会话累积，不消费（tee₁ 于会话末取全量；重放幂等）。
 **/

#pragma once

// LOVE
#include "common/config.h"
#include "common/int.h"
#include "common/Matrix.h"
#include "modules/graphics/Graphics.h"

#include <string>
#include <vector>

struct lua_State;

namespace fr
{

// 资产种类口径（l0/fr_ops.lua kinds / fr_codec ASSET 常量同源，勿改数值）。
enum FrAssetKind
{
	FR_ASSET_IMAGE  = 0,
	FR_ASSET_CANVAS = 1,
	FR_ASSET_QUAD   = 2,
	FR_ASSET_FONT   = 3
};

// FR_CAPTURE=1 开启；env 只在首次调用读一次并缓存（启动口径）。
bool capture_enabled();

// w_clear 挂钩记录点：真执行的抑制由调用方（wrap_Graphics.cpp）提前 return
// 负责，本函数只记录。颜色分量域 0-1 浮点 → rgba8 整数。
void capture_clear(const love::graphics::OptionalColorf &color,
                   const std::vector<love::graphics::OptionalColorf> &colors);

// Task 3 捕获点：坐标已由 wrap 层经 getTransform 复合结算（世界系），颜色
// 取捕获时刻的 getColor() 状态（L0 行内 color = setColor 折参数，cmd.tbl:33）。
void capture_draw(love::uint32 texid, love::uint32 quadid, float x, float y, float rot,
                  float sx, float sy, float ox, float oy, const love::Colorf &color);
void capture_print(love::uint32 fontid, const std::string &text, float x, float y,
                   float rot, float sx, float sy, const love::Colorf &color);
void capture_rect(love::uint8 mode, float x, float y, float w, float h, float rx,
                  float ry, float width, const love::Colorf &color);

// 回放：遍历行缓冲，逐行调 Graphics 的 C++ 接口（与 w_* 同一 instance()
// 方法，绕过 Lua 边界）。不消费缓冲。缓冲空 = no-op（开关关时安全可调）。
void capture_replay(love::graphics::Graphics *gfx);

// out 段行文本（draw/print/rect/clear）。
std::vector<std::string> capture_take_rows();
// 资产行文本（登记序，Task 4 会话组装按 frame 0 checkpoint 注入）。
std::vector<std::string> fr_take_asset_rows();

// 物化侧查表（fr_materialize 用）：登记资产的只读拷贝视图。data_hex 空 =
// 登记时无字节（"-" 占位，物化不可用）。image 字节口径 = RGBA8 像素
// （协议约定，非 RGBA8 源资产归后续批次）；font 字节 = TrueType 数据。
struct FrAssetInfo
{
	int kind = 0;
	int w = 0, h = 0;                 // image / canvas
	float size = 0.0f;                // font
	std::string data_hex;             // image / font 真字节 hex（空 = "-"）
};

// 按 id 查登记资产（未登记 → false）。
bool fr_lookup_asset(love::uint32 id, FrAssetInfo &out);

// 语义替代四件套（w_new* 挂钩转入，开关开时生效）：
//   newImage(filename|ImageData) → 登记真字节 hex（文件字节 / ImageData 像素）
//   newFont(size)                → size 登记（f32 口径）+ Vera.ttf 真字节
//   newCanvas(w, h)              → 登记（不建真 Canvas，见文件头边界）
//   newQuad(x, y, w, h, sw, sh)  → 登记
// 返回资产代理 userdata。
int w_frNewImage(lua_State *L);
int w_frNewFont(lua_State *L);
int w_frNewCanvas(lua_State *L);
int w_frNewQuad(lua_State *L);

// 代理判定/取号：idx 是 fr 资产代理 → true / 资产号（非代理返回 0，
// 调用方先用 fr_isasset 判定——0 号是空资产哨兵，不可撞）。
bool fr_isasset(lua_State *L, int idx);
love::uint32 fr_assetid(lua_State *L, int idx);

// Lua 出口：
//   love.graphics._frTakeSession() → out 段行文本数组；
//   love.graphics._frTakeAssets()  → 资产行文本数组；
//   love.graphics._frReplay()      → 对当前 Graphics 实例回放一次；
//   love.graphics._frNewCanvasReal(w, h) → 真 Canvas（开关无关；验证帧
//       回读 rig 专用——语义替代版不建真 Canvas，见文件头 Canvas 边界）。
int w_frTakeSession(lua_State *L);
int w_frTakeAssets(lua_State *L);
int w_frReplay(lua_State *L);
int w_frNewCanvasReal(lua_State *L);

} // fr
