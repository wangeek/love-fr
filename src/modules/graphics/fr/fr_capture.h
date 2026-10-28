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

namespace love
{
class Variant;
}

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

// canonical 文本转义（\ " \n \r \t，fr.esc 同构）——捕获行与 fr_statediff 的
// st/err 行共用（单一格式源；控制字符校验归 fr_check 解析侧，demo 文本受控）。
std::string fr_esc_text(const std::string &s);

// CRC32（l0/walker.lua 同构：查表 0xEDB88320、reflected、init/xorout
// 0xFFFFFFFF；空输入 = 0）。一次性外壳为 D_CRC 对拍锚（live 验证帧与
// 离线回放同一采集语义，勿改）；增量入口供 fr_statediff 逐段混入（同
// 一查表实现——堆 CRC 与 D_CRC 同一 CRC32 机）。
love::uint32 fr_crc32_update(love::uint32 state, const void *data, size_t n);
love::uint32 fr_crc32_bytes(const void *data, size_t n);

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

// B9：present 上屏增量重放（游标式；性能——全量成本随会话行数二次方，
// 真实游戏 2500+ 行/帧实测爆炸）。验证 Canvas 场景仍走全量版。
void capture_replay_delta(love::graphics::Graphics *gfx);

// 单行执行（capture_replay 的 per-row 内核）：拓扑 B 离线回放与 tee₂ 回放
// 共用同一行执行入口（物化器/真渲染同构的关键，B4 阶段三 Task 3）。
// B8 T3：rt 0 的"屏幕" = 回放会话基线目标（两回放驱动进出配对设置）。
void capture_exec_row(love::graphics::Graphics *gfx, const std::string &row);

// B8 T3：观察侧围栏——suspend 计数 >0 时 capture_enabled()=false（挂钩全
// 旁路=真执行零记录）。验证帧 rig 的真 Canvas 切换是观察机器，不进内核流。
void capture_suspend();
void capture_resume();

// B10：C 侧 in 段捕获（任意游戏零改动出流的 ev 事实源）。src=0 OS 路径
//（Event::pump convert 点）/1 注入路径（w_push）。白名单内离散事件记行
//（与 demo Lua 侧 IN_EVENTS 同集），slot=帧内取号序号；writer 在 present
// 冲帧后重置计数。capture 关闭时 no-op。
void capture_ev(int src, const std::string &name, const std::vector<love::Variant> &args);

// B10：帧头 dt 事实源——w_step 钩记录当帧 step() 返回值（默认 run loop
// 每帧恰一次）；writer 在 present 取走（取走即失效）。
void fr_set_pending_dt(double dt);
bool fr_take_pending_dt(double &out);

// B9：SEH 崩溃探针（仅 Windows；FR_CRASH_PROBE=1 时由 present 钩懒装）。
void fr_install_crash_probe();

// out 段行文本（draw/print/rect/clear）。
std::vector<std::string> capture_take_rows();
// 捕获期 err 行（Shader 边界等，log 段语义；累积拷贝，demo 切 delta）。
std::vector<std::string> fr_take_err_rows();

// B10：writer 只读视图（fr_session 冲帧用；游标归 writer 自持）。
const std::vector<std::string> &fr_view_rows();      // out 段
const std::vector<std::string> &fr_view_err_rows();  // 捕获期 err（log 段）
const std::vector<std::string> &fr_view_ev_rows();   // in 段（B10 C 侧捕获）
void fr_ev_reset_slot();                             // 每帧冲帧后 slot 归零
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

// 流内资产登记（拓扑 B 离线回放用）：id 取流行（不重编号），只入登记表
// 供物化器查表——不入 asset_rows（离线消费流，不回产会话文本）。
// quad 可为 nullptr（非 quad 资产）；data_hex 空 = "-" 占位（物化不可用）。
bool fr_register_stream_asset(int kind, love::uint32 id, int w, int h, float size,
                              const float *quad, const std::string &data_hex);

// 拓扑 B 离线回放（B4 阶段三 Task 3）：读 .fr/.frb 会话文件 → 逐帧经
// capture_exec_row 执行（与 tee₂ 同一行执行路径）→ 每帧末真 Canvas 回读
// ImageData → CRC32 → stdout 打印 `D_CRC <n> <crc>`（n = 已执行帧块数，
// live 验证帧计数同口径）；末帧加三采样 PIXEL 行（live 验证帧同点同换算）。
// 判 magic：FRB1 = 二进制（会话封装解包 + fr_dec 行解码），否则文本按行。
// 返回 0 成功；非 0 = 错误（细节已写 stderr，含偏移 off / 行号）。
// L 供 FR_PLAY=1 视觉播放的 present（默认判据路径不用）。
int fr_offline_replay(lua_State *L, const char *path, love::graphics::Graphics *gfx);

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

// B8 T1 字体状态跟踪：w_setFont 挂钩在真 setFont 之外，参数为代理时记
// 其 id；w_print 未传 font（无代理形参）时回退该状态（原生游戏形态：
// love.graphics.setFont(f) + love.graphics.print(text, x, y)）。
void fr_set_current_font(love::uint32 id);
love::uint32 fr_current_font();

// B8 T2 绘制类五 op 捕获（cmd.tbl 封版签名；坐标经 getTransform 结算——
// 调用方（w_* 钩）传已结算的世界系坐标）：
void capture_line(const float *pts, love::uint32 n, float width, const love::Colorf &color);
void capture_circle(love::uint8 mode, float x, float y, float r, love::uint32 seg,
                    const love::Colorf &color);
void capture_poly(love::uint8 mode, const float *pts, love::uint32 n,
                  const love::Colorf &color);
void capture_arc(love::uint8 mode, love::uint8 arctype, float x, float y, float r,
                 float a1, float a2, love::uint32 seg, const love::Colorf &color);
void capture_points(const float *pts, love::uint32 n, float size, const love::Colorf &color);

// B8 T3 目标/裁剪/Shader 边界：
void capture_rt(love::uint32 target);          // setCanvas → rt 行（0=屏幕）
void capture_scissor(float x, float y, float w, float h); // -1 哨兵=off
void capture_shader_boundary();                 // setShader → err 行（语义边界）
int fr_asset_kind_of(love::uint32 id);          // 资产号 → kind（0=未登记）
void capture_api_boundary(const char *what);    // B9：参数化 err 边界（blendmode/printf…）

// Lua 出口：
//   love.graphics._frTakeSession() → out 段行文本数组；
//   love.graphics._frTakeAssets()  → 资产行文本数组；
//   love.graphics._frReplay()      → 对当前 Graphics 实例回放一次；
//   love.graphics._frNewCanvasReal(w, h) → 真 Canvas（开关无关；验证帧
//       回读 rig 专用——语义替代版不建真 Canvas，见文件头 Canvas 边界）；
//   love.graphics._frImageDataCRC(img) → ImageData 全字节 CRC32（live
//       验证帧 D_CRC 与离线回放同一采集函数，对拍锚）；
//   love.graphics._frCRC32(s) → 字符串字节 CRC32（u32）——B7a d_crc 行的
//       数据源出口：demo 侧拼当帧 out 行文本（take_delta 同批行 join
//       "\n"）传入、C 侧算（拼串归调用方、算法单一真源在 C——与 D_CRC/
//       s_crc 同一 CRC32 机）；
//   love.graphics._frCanvasProbe(canvas[, frame]) → 验证帧三点像素回读
//       + PIXELS 汇总 + D_CRC（B6：整环 C 侧 printf，stdout 行与原 Lua
//       环逐字节同构——Lua 侧 getPixel/format/print 在验证帧留 live 残留
//       表，污染末帧 s_crc 的窗口/无头一致性，实测定位）；
//   love.graphics._frOfflineReplay(path) → fr_offline_replay 的 rc。
int w_frTakeSession(lua_State *L);
int w_frTakeErrRows(lua_State *L);          // B8 T4：捕获期 err 行（log 段组装/sim 比对）
int w_frCaptureSuspend(lua_State *L);   // B8 T3：观察侧围栏（配对 resume）
int w_frCaptureResume(lua_State *L);
int w_frSessionFrame(lua_State *L);     // B10：显式补帧口（验证帧等价点）
int w_frTakeAssets(lua_State *L);
int w_frReplay(lua_State *L);
int w_frNewCanvasReal(lua_State *L);
int w_frImageDataCRC(lua_State *L);
int w_frCRC32(lua_State *L);
int w_frCanvasProbe(lua_State *L);
int w_frOfflineReplay(lua_State *L);

} // fr
