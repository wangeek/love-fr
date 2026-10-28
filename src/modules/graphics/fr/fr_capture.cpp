/**
 * FR 捕获基础设施实现（B4-2 fork 魔改）——边界与行格式见 fr_capture.h。
 **/

#include "modules/graphics/fr/fr_capture.h"
#include "modules/graphics/fr/fr_materialize.h"
#include "modules/graphics/fr/fr_session.h"

// LOVE
#include "common/Exception.h"
#include "common/Module.h"
#include "common/runtime.h"
#include "common/Variant.h"
#include "filesystem/Filesystem.h"
#include "graphics/Canvas.h"
#include "graphics/Font.h"
#include "image/Image.h"
#include "image/ImageData.h"
#include "timer/Timer.h"
#include "modules/graphics/fr/fr_codec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <cstring>

#ifdef _WIN32
// B9 崩溃探针（FR_CRASH_PROBE=1）：SEH 未处理异常过滤器 + dbghelp 符号
// 解析（Release 构建带 PDB）——崩溃现场直打函数名+栈回溯，免外部调试器
//（本机 lldb Python 桥坏/cdb 未装的三缺现场）。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace fr
{

static void fr_sym_line(HANDLE proc, DWORD64 addr)
{
	// 模块名 + 模块内 RVA（私有符号解不出时保底——llvm-symbolizer 离线解）。
	HMODULE mod = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
	                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       (LPCWSTR) (uintptr_t) addr, &mod) && mod != nullptr)
	{
		wchar_t wpath[MAX_PATH];
		GetModuleFileNameW(mod, wpath, MAX_PATH);
		char path[MAX_PATH * 2];
		WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, sizeof path, nullptr, nullptr);
		const char *base = strrchr(path, '\\');
		base = base != nullptr ? base + 1 : path;
		std::fprintf(stderr, "  %s RVA 0x%llx", base,
		             (unsigned long long) (addr - (DWORD64) (uintptr_t) mod));
	}
	SYMBOL_INFO *sym = (SYMBOL_INFO *) calloc(sizeof(SYMBOL_INFO) + 256, 1);
	if (sym != nullptr)
	{
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 disp = 0;
		if (SymFromAddr(proc, addr, &disp, sym))
			std::fprintf(stderr, " = %s +0x%llx\n", sym->Name, (unsigned long long) disp);
		else
			std::fprintf(stderr, "\n");
		free(sym);
	}
}

static LONG WINAPI fr_crash_filter(EXCEPTION_POINTERS *ep)
{
	HANDLE proc = GetCurrentProcess();
	SymInitialize(proc, nullptr, TRUE);
	std::fprintf(stderr, "FR_CRASH code=0x%lx addr=0x%llx\n",
	             ep->ExceptionRecord->ExceptionCode,
	             (unsigned long long) (uintptr_t) ep->ExceptionRecord->ExceptionAddress);
	fr_sym_line(proc, (DWORD64) (uintptr_t) ep->ExceptionRecord->ExceptionAddress);
	void *frames[10];
	WORD n = CaptureStackBackTrace(0, 10, frames, nullptr);
	std::fprintf(stderr, "FR_CRASH backtrace %u frames:\n", (unsigned) n);
	for (WORD i = 0; i < n; i++)
		fr_sym_line(proc, (DWORD64) (uintptr_t) frames[i]);
	std::fflush(stderr);
	return EXCEPTION_CONTINUE_SEARCH;
}

void fr_install_crash_probe()
{
	static bool installed = false;
	if (!installed)
	{
		SetUnhandledExceptionFilter(fr_crash_filter);
		installed = true;
	}
}

} // namespace fr
#endif

// modules/font/Font.cpp 的默认字体：在 love::font 命名空间内 include
// Vera.ttf.h（内嵌 TTF，仅该 TU include）——符号归属该命名空间，登记真
// 字节直接外部引用（newFont(size) 的默认字体即它）。
namespace love
{
namespace font
{
extern unsigned char Vera_ttf[];
extern unsigned int Vera_ttf_len;
} // font
} // love

namespace fr
{

// ------------------------------------------------------------------
// 会话行缓冲（out 段 / 资产段）
// ------------------------------------------------------------------

static std::vector<std::string> &rows()
{
	static std::vector<std::string> r;
	return r;
}

static std::vector<std::string> &asset_rows()
{
	static std::vector<std::string> r;
	return r;
}

// B8 T3：观察侧围栏计数——>0 时挂钩全旁路（真执行、零记录）。验证帧 rig
// 的真 Canvas 切换（setCanvas(c)/setCanvas()）是观察机器，不得进内核会话流
//（多 VM 原则：观察机器不进被观测流）。
static int g_suspend = 0;

// B8 T4：捕获期 err 行（Shader 边界等）独立缓冲——err 属 log 段（B6 行序
// dt→asset→ev→st/err→out；err 豁免于 off/base 档），不混 out 捕获流
//（d_crc 口径 = 纯 out op，statediff 的 err 行同此纪律）。
static std::vector<std::string> &err_rows()
{
	static std::vector<std::string> r;
	return r;
}

bool capture_enabled()
{
	static const bool enabled = []()
	{
		const char *v = getenv("FR_CAPTURE");
		return v != nullptr && std::string(v) == "1";
	}();
	return enabled && g_suspend == 0;
}

void capture_suspend()
{
	g_suspend++;
}

void capture_resume()
{
	if (g_suspend > 0)
		g_suspend--;
}

// ------------------------------------------------------------------
// B10：C 侧 in 段捕获 + 帧头 dt 事实源（任意游戏零改动出流）
// ------------------------------------------------------------------

static std::vector<std::string> &ev_rows()
{
	static std::vector<std::string> r;
	return r;
}

static int g_ev_slot = 0; // 帧内取号序号（writer 冲帧后归零）

static double g_pending_dt = 0.0;
static bool g_dt_fresh = false;

void fr_set_pending_dt(double dt)
{
	g_pending_dt = dt;
	g_dt_fresh = true;
}

bool fr_take_pending_dt(double &out)
{
	if (!g_dt_fresh)
		return false;
	out = g_pending_dt;
	g_dt_fresh = false;
	return true;
}

const std::vector<std::string> &fr_view_rows() { return rows(); }
const std::vector<std::string> &fr_view_err_rows() { return err_rows(); }
const std::vector<std::string> &fr_view_ev_rows() { return ev_rows(); }

void fr_ev_reset_slot()
{
	g_ev_slot = 0;
}

// in 段白名单（与 demo main.lua IN_EVENTS 同集——环境/连续事件不记：
// mousemoved/focus/visible 随窗口态漂移，进流破 tee 确定性，B5 实测口径）。
static bool ev_whitelisted(const std::string &name)
{
	return name == "keypressed" || name == "keyreleased" || name == "textinput"
	    || name == "mousepressed" || name == "mousereleased" || name == "wheelmoved";
}

// Variant → token（demo tok_variant 同构：num %.6f / str "<esc>" / bool 0|1）。
// 非 num/str/bool 形态（白名单事件不会出现）→ false（调用方跳过整事件）。
static bool ev_variant_token(const love::Variant &v, std::string &tok)
{
	switch (v.getType())
	{
	case love::Variant::BOOLEAN:
		tok = v.getData().boolean ? "bool 1" : "bool 0";
		return true;
	case love::Variant::NUMBER:
	{
		double d = v.getData().number;
		if (d != d || d == HUGE_VAL || d == -HUGE_VAL)
			return false; // NaN/Inf 禁入数值域（fr-spec）
		char buf[64];
		snprintf(buf, sizeof(buf), "num %.6f", d);
		tok = buf;
		return true;
	}
	case love::Variant::STRING:
	{
		const love::Variant::SharedString *s = v.getData().string;
		tok = "str \"" + fr_esc_text(std::string(s->str, s->len)) + "\"";
		return true;
	}
	case love::Variant::SMALLSTRING:
	{
		const auto &ss = v.getData().smallstring;
		tok = "str \"" + fr_esc_text(std::string(ss.str, ss.len)) + "\"";
		return true;
	}
	default:
		return false;
	}
}

void capture_ev(int src, const std::string &name, const std::vector<love::Variant> &args)
{
	if (!capture_enabled() || !ev_whitelisted(name))
		return;

	// 尾部 NIL/UNKNOWN 截断（demo argc 口径 = 最后一个非 nil 实参）；中间
	// nil 是流格式无法表达的形态（白名单事件不应出现）——整事件跳过+告警。
	size_t argc = args.size();
	while (argc > 0 && (args[argc - 1].getType() == love::Variant::NIL
	                    || args[argc - 1].getType() == love::Variant::UNKNOWN))
		argc--;
	std::string row = "ev " + std::to_string(g_ev_slot++)
	                  + " \"" + fr_esc_text(name) + "\" " + std::to_string(src)
	                  + " " + std::to_string(argc);
	for (size_t i = 0; i < argc; i++)
	{
		std::string tok;
		if (!ev_variant_token(args[i], tok))
		{
			static bool warned = false;
			if (!warned)
			{
				std::fprintf(stderr, "fr: ev %s 含不可序列化实参（nil/对象），整行跳过\n",
				             name.c_str());
				warned = true;
			}
			g_ev_slot--;
			return;
		}
		row += " " + tok;
	}
	ev_rows().push_back(row);
}

// ------------------------------------------------------------------
// 行文本工具（fr.lua 同构）
// ------------------------------------------------------------------

static int rgba8(float v)
{
	if (v < 0.0f)
		v = 0.0f;
	if (v > 1.0f)
		v = 1.0f;
	return (int) lroundf(v * 255.0f);
}

// fr.esc 同构：\ " \n \r \t 转义（控制字符拒收在 Lua 侧 render 纪律内，
// 本层只转义不校验——demo 文本受控，完整校验归 fr_check 解析侧）。
static std::string esc(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s)
	{
		switch (c)
		{
		case '\\': out += "\\\\"; break;
		case '"': out += "\\\""; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default: out += c;
		}
	}
	return out;
}

std::string fr_esc_text(const std::string &s)
{
	return esc(s);
}

// esc 的逆（replay 行解析用）：\\ \" \n \r \t 展开为原始字节；非法转义
//（esc 不产的形态）原样保留两字符——原文本含 `\x` 时 esc 产 `\\x`，逆展开
// 得回 `\x`，round-trip 保真。
static std::string unesc(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] != '\\' || i + 1 >= s.size())
		{
			out += s[i];
			continue;
		}
		char c = s[++i];
		switch (c)
		{
		case '\\': out += '\\'; break;
		case '"': out += '"'; break;
		case 'n': out += '\n'; break;
		case 'r': out += '\r'; break;
		case 't': out += '\t'; break;
		default: out += '\\'; out += c; break;
		}
	}
	return out;
}

// 字节 → 小写 hex（资产行 data 段；fr.lua 的 hex 编码同构）。
static std::string hex_encode(const void *bytes, size_t n)
{
	static const char *hexd = "0123456789abcdef";
	const love::uint8 *p = (const love::uint8 *) bytes;
	std::string out;
	out.reserve(n * 2);
	for (size_t i = 0; i < n; i++)
	{
		out += hexd[p[i] >> 4];
		out += hexd[p[i] & 0xF];
	}
	return out;
}

// ------------------------------------------------------------------
// 行文本渲染（捕获 emit 与拓扑 B 二进制解码重渲染共用——单一格式源，
// fr-as/fr-dis 的 canonical 往返由根仓库回归守护）
// ------------------------------------------------------------------

static std::string render_clear_row(int r, int g, int b, int a)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "clear %d %d %d %d", r, g, b, a);
	return buf;
}

static std::string render_draw_row(love::uint32 tex, love::uint32 quad, float x, float y,
                                   float rot, float sx, float sy, float ox, float oy,
                                   int r, int g, int b, int a)
{
	char buf[256];
	snprintf(buf, sizeof(buf),
	         "draw %u %u %.6f %.6f %.6f %.6f %.6f %.6f %.6f %d %d %d %d",
	         tex, quad, x, y, rot, sx, sy, ox, oy, r, g, b, a);
	return buf;
}

static std::string render_print_row(love::uint32 font, const std::string &text, float x, float y,
                                    float rot, float sx, float sy, int r, int g, int b, int a)
{
	char buf[512];
	snprintf(buf, sizeof(buf),
	         "print %u \"%s\" %.6f %.6f %.6f %.6f %.6f %d %d %d %d",
	         font, esc(text).c_str(), x, y, rot, sx, sy, r, g, b, a);
	return buf;
}

static std::string render_rect_row(love::uint8 mode, float x, float y, float w, float h,
                                   float rx, float ry, float width, int r, int g, int b, int a)
{
	char buf[256];
	snprintf(buf, sizeof(buf),
	         "rect %u %.6f %.6f %.6f %.6f %.6f %.6f %.6f %d %d %d %d",
	         (unsigned) mode, x, y, w, h, rx, ry, width, r, g, b, a);
	return buf;
}

// B8 T3：rt/scissor 行文本（捕获与二进制离线重渲染共用——单一产行源）。
static std::string render_rt_row(love::uint32 target)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "rt %u", (unsigned) target);
	return buf;
}

static std::string render_scissor_row(float x, float y, float w, float h)
{
	char buf[128];
	snprintf(buf, sizeof(buf), "scissor %.6f %.6f %.6f %.6f", x, y, w, h);
	return buf;
}

// ------------------------------------------------------------------
// 捕获点（clear + Task 3 draw/print/rect）
// ------------------------------------------------------------------

void capture_clear(const love::graphics::OptionalColorf &color,
                   const std::vector<love::graphics::OptionalColorf> &colors)
{
	const love::graphics::OptionalColorf *c = nullptr;

	if (!colors.empty())
	{
		if (colors[0].hasValue)
			c = &colors[0];
	}
	else if (color.hasValue)
	{
		c = &color;
	}

	if (c == nullptr)
		return; // 无颜色 clear（clear(false)/纯 stencil/depth）本批不产行

	rows().push_back(render_clear_row(rgba8(c->value.r), rgba8(c->value.g),
	                                  rgba8(c->value.b), rgba8(c->value.a)));
}

void capture_draw(love::uint32 texid, love::uint32 quadid, float x, float y, float rot,
                  float sx, float sy, float ox, float oy, const love::Colorf &color)
{
	rows().push_back(render_draw_row(texid, quadid, x, y, rot, sx, sy, ox, oy,
	                                 rgba8(color.r), rgba8(color.g),
	                                 rgba8(color.b), rgba8(color.a)));
}

void capture_print(love::uint32 fontid, const std::string &text, float x, float y,
                   float rot, float sx, float sy, const love::Colorf &color)
{
	rows().push_back(render_print_row(fontid, text, x, y, rot, sx, sy,
	                                  rgba8(color.r), rgba8(color.g),
	                                  rgba8(color.b), rgba8(color.a)));
}

void capture_rect(love::uint8 mode, float x, float y, float w, float h, float rx,
                  float ry, float width, const love::Colorf &color)
{
	rows().push_back(render_rect_row(mode, x, y, w, h, rx, ry, width,
	                                 rgba8(color.r), rgba8(color.g),
	                                 rgba8(color.b), rgba8(color.a)));
}

// B8 T2 绘制类五 op（cmd.tbl 封版签名；行格式与 render_line/circle 等
// Python 侧 canonical 对齐——f32 恒 %.6f、pts 前带点数）。
static std::string fmt_f(float v)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%.6f", v);
	return buf;
}

static std::string render_pts(const float *pts, love::uint32 n)
{
	std::string s = std::to_string(n);
	for (love::uint32 i = 0; i < n * 2; i++)
		s += " " + fmt_f(pts[i]);
	return s;
}

void capture_line(const float *pts, love::uint32 n, float width, const love::Colorf &color)
{
	std::string row = "line " + render_pts(pts, n)
		+ " " + fmt_f(width)
		+ " " + std::to_string(rgba8(color.r)) + " " + std::to_string(rgba8(color.g))
		+ " " + std::to_string(rgba8(color.b)) + " " + std::to_string(rgba8(color.a));
	rows().push_back(row);
}

void capture_circle(love::uint8 mode, float x, float y, float r, love::uint32 seg,
                    const love::Colorf &color)
{
	char buf[160];
	snprintf(buf, sizeof(buf), "circle %u %s %s %s %u %d %d %d %d",
	         (unsigned) mode, fmt_f(x).c_str(), fmt_f(y).c_str(), fmt_f(r).c_str(),
	         (unsigned) seg, rgba8(color.r), rgba8(color.g), rgba8(color.b), rgba8(color.a));
	rows().push_back(buf);
}

void capture_poly(love::uint8 mode, const float *pts, love::uint32 n,
                  const love::Colorf &color)
{
	std::string row = "poly " + std::to_string((unsigned) mode) + " " + render_pts(pts, n)
		+ " " + std::to_string(rgba8(color.r)) + " " + std::to_string(rgba8(color.g))
		+ " " + std::to_string(rgba8(color.b)) + " " + std::to_string(rgba8(color.a));
	rows().push_back(row);
}

void capture_arc(love::uint8 mode, love::uint8 arctype, float x, float y, float r,
                 float a1, float a2, love::uint32 seg, const love::Colorf &color)
{
	char buf[200];
	snprintf(buf, sizeof(buf), "arc %u %u %s %s %s %s %s %u %d %d %d %d",
	         (unsigned) mode, (unsigned) arctype,
	         fmt_f(x).c_str(), fmt_f(y).c_str(), fmt_f(r).c_str(),
	         fmt_f(a1).c_str(), fmt_f(a2).c_str(), (unsigned) seg,
	         rgba8(color.r), rgba8(color.g), rgba8(color.b), rgba8(color.a));
	rows().push_back(buf);
}

void capture_points(const float *pts, love::uint32 n, float size, const love::Colorf &color)
{
	std::string row = "points " + render_pts(pts, n)
		+ " " + fmt_f(size)
		+ " " + std::to_string(rgba8(color.r)) + " " + std::to_string(rgba8(color.g))
		+ " " + std::to_string(rgba8(color.b)) + " " + std::to_string(rgba8(color.a));
	rows().push_back(row);
}

void capture_rt(love::uint32 target)
{
	rows().push_back(render_rt_row(target));
}

void capture_scissor(float x, float y, float w, float h)
{
	rows().push_back(render_scissor_row(x, y, w, h));
}

void capture_shader_boundary()
{
	// err 行进独立缓冲（log 段语义）——不出现在 out 捕获流（take_delta/d_crc）。
	err_rows().push_back("err \"st-unsupported shader no-ir（语义边界，真执行不抑制）\"");
}

// B9：参数化语义边界（setBlendMode/printf 等——无 IR op 的渲染语义 API，
// 捕获态零真执行纪律下的如实登记；回放侧画面承受相应语义缺口）。
void capture_api_boundary(const char *what)
{
	std::string row = "err \"st-unsupported ";
	row += what;
	row += " no-ir（语义边界）\"";
	err_rows().push_back(row);
}

// ------------------------------------------------------------------
// 资产登记表 + 资产代理（语义替代四件套）
// ------------------------------------------------------------------

struct FrAsset
{
	int kind = 0;
	love::uint32 id = 0;
	int w = 0, h = 0;                 // image / canvas
	float size = 0.0f;                // font
	float qx = 0, qy = 0, qw = 0, qh = 0, qsw = 0, qsh = 0; // quad
	std::string data_hex;             // image / font（空 = "-"）
	// B9 代理态（不进协议行）：采样参数属渲染实现细节（D10），代理方法
	// setFilter/getFilter 的承载；默认 linear/linear（11.5 新图缺省同 love
	// 缺省 filter 族）。
	std::string filter_min = "linear", filter_mag = "linear";
};

static std::vector<FrAsset> &assets()
{
	static std::vector<FrAsset> a;
	return a;
}

static love::uint32 next_asset_id()
{
	static love::uint32 id = 0;
	return ++id; // 资产号自 1 起（0 = 空资产哨兵）
}

static const char *kind_name(int kind)
{
	static const char *names[] = { "image", "canvas", "quad", "font" };
	return names[kind];
}

// fr.render_asset 同构（资产行文本，登记序入 asset_rows）。data 段可为
// 任意长 hex（image 像素 / font TTF 字节）——std::string 拼接，不走定长
// 缓冲（B4 阶段三实测：128B snprintf 截断 8192 字符 hex）。
static std::string render_asset_row(const FrAsset &a)
{
	char head[128];
	if (a.kind == FR_ASSET_CANVAS)
	{
		snprintf(head, sizeof(head), "asset canvas %u %d %d", a.id, a.w, a.h);
		return head;
	}
	if (a.kind == FR_ASSET_QUAD)
	{
		snprintf(head, sizeof(head), "asset quad %u %.6f %.6f %.6f %.6f %.6f %.6f",
		         a.id, a.qx, a.qy, a.qw, a.qh, a.qsw, a.qsh);
		return head;
	}
	if (a.kind == FR_ASSET_IMAGE)
		snprintf(head, sizeof(head), "asset image %u %d %d ", a.id, a.w, a.h);
	else
		snprintf(head, sizeof(head), "asset font %u %.6f ", a.id, a.size);
	return std::string(head) + (a.data_hex.empty() ? "-" : a.data_hex);
}

// 资产代理：full userdata + 专属元表（11.5 wrap 惯例同构；弃 lightuserdata
// 方案的理由见 fr_capture.h）。载荷 {id, kind} 两字段只读。
struct FrProxy
{
	love::uint32 id;
	int kind;
};

// ------------------------------------------------------------------
// B9 代理方法族（真实游戏兼容——对象方法调用面 30+ 处：img:getWidth/
// getDimensions、font:getWidth(text)/getHeight 等，遍布 modal/world_map/
// battle/grotto/grid）。分派按 kind：
//   image/canvas 几何族 = 登记几何直读（零 GPU）；
//   font 度量族 = 物化真 Font（与回放侧同一物化器，会话缓存）调真方法
//   ——捕获进程有窗 = 有 GL；无头录制（拓扑 C）物化失败回退 size 线性
//   估算 + stderr 一次性告警（游戏布局计算可用，精度如实标注）。
//   filter 族 = 代理态（D10：采样参数属渲染实现，不进协议行）。
// ------------------------------------------------------------------

static FrAsset *fr_find_asset(love::uint32 id)
{
	for (auto &a : assets())
		if (a.id == id)
			return &a;
	return nullptr;
}

static FrProxy *proxy_arg(lua_State *L)
{
	return (FrProxy *) lua_touserdata(L, 1);
}

static FrAsset *proxy_asset(lua_State *L)
{
	FrProxy *p = proxy_arg(L);
	return p != nullptr ? fr_find_asset(p->id) : nullptr;
}

static int pm_getWidth(lua_State *L)
{
	FrAsset *a = proxy_asset(L);
	if (a != nullptr && a->kind == FR_ASSET_FONT)
	{
		// font:getWidth(text)——真排版宽度（物化真 Font）
		const char *text = luaL_checkstring(L, 2);
		love::graphics::Font *f =
			fr_materialize_font(love::Module::getInstance<love::graphics::Graphics>(
				love::Module::M_GRAPHICS), a->id);
		if (f != nullptr)
		{
			lua_pushnumber(L, (lua_Number) f->getWidth(text));
			return 1;
		}
		static bool warned = false;
		if (!warned)
		{
			std::fprintf(stderr, "fr: font 度量物化失败（无头？），getWidth 回退线性估算\n");
			warned = true;
		}
		lua_pushnumber(L, (lua_Number) (strlen(text) * a->size * 0.6));
		return 1;
	}
	lua_pushnumber(L, a != nullptr ? (lua_Number) a->w : 0);
	return 1;
}

static int pm_getHeight(lua_State *L)
{
	FrAsset *a = proxy_asset(L);
	if (a != nullptr && a->kind == FR_ASSET_FONT)
	{
		love::graphics::Font *f =
			fr_materialize_font(love::Module::getInstance<love::graphics::Graphics>(
				love::Module::M_GRAPHICS), a->id);
		if (f != nullptr)
		{
			lua_pushnumber(L, (lua_Number) f->getHeight());
			return 1;
		}
		static bool warned = false;
		if (!warned)
		{
			std::fprintf(stderr, "fr: font 度量物化失败（无头？），getHeight 回退 size*1.25\n");
			warned = true;
		}
		lua_pushnumber(L, (lua_Number) (a->size * 1.25f));
		return 1;
	}
	lua_pushnumber(L, a != nullptr ? (lua_Number) a->h : 0);
	return 1;
}

// font 度量族其余成员（ascent/descent/baseline/lineheight 同 getHeight 形）
enum FontMetric
{
	FM_LINEHEIGHT, FM_ASCENT, FM_DESCENT, FM_BASELINE
};

static int pm_font_metric(lua_State *L, FontMetric which, const char *name, float fallback)
{
	FrAsset *a = proxy_asset(L);
	love::graphics::Font *f =
		fr_materialize_font(love::Module::getInstance<love::graphics::Graphics>(
			love::Module::M_GRAPHICS), a != nullptr ? a->id : 0);
	lua_Number v = fallback;
	if (f != nullptr)
	{
		switch (which)
		{
		case FM_LINEHEIGHT: v = (lua_Number) f->getLineHeight(); break;
		case FM_ASCENT:     v = (lua_Number) f->getAscent(); break;
		case FM_DESCENT:    v = (lua_Number) f->getDescent(); break;
		case FM_BASELINE:   v = (lua_Number) f->getBaseline(); break;
		}
	}
	else
	{
		static bool warned = false;
		if (!warned)
		{
			std::fprintf(stderr, "fr: font 度量物化失败（无头？），%s 回退线性估算\n", name);
			warned = true;
		}
	}
	lua_pushnumber(L, v);
	return 1;
}

static int pm_getLineHeight(lua_State *L)
{
	return pm_font_metric(L, FM_LINEHEIGHT, "getLineHeight",
	                      proxy_asset(L) != nullptr ? proxy_asset(L)->size * 1.25f : 0);
}

static int pm_getAscent(lua_State *L)
{
	return pm_font_metric(L, FM_ASCENT, "getAscent",
	                      proxy_asset(L) != nullptr ? proxy_asset(L)->size : 0);
}

static int pm_getDescent(lua_State *L)
{
	return pm_font_metric(L, FM_DESCENT, "getDescent",
	                      proxy_asset(L) != nullptr ? proxy_asset(L)->size * 0.25f : 0);
}

static int pm_getBaseline(lua_State *L)
{
	return pm_font_metric(L, FM_BASELINE, "getBaseline",
	                      proxy_asset(L) != nullptr ? proxy_asset(L)->size : 0);
}

static int pm_getDimensions(lua_State *L)
{
	FrAsset *a = proxy_asset(L);
	lua_pushnumber(L, a != nullptr ? (lua_Number) a->w : 0);
	lua_pushnumber(L, a != nullptr ? (lua_Number) a->h : 0);
	return 2;
}

static int pm_getPixelWidth(lua_State *L)
{
	FrProxy *p = proxy_arg(L);
	FrAsset *a = fr_find_asset(p != nullptr ? p->id : 0);
	lua_pushnumber(L, a != nullptr ? (lua_Number) a->w : 0); // dpiScale=1 口径
	return 1;
}

static int pm_getPixelHeight(lua_State *L)
{
	FrProxy *p = proxy_arg(L);
	FrAsset *a = fr_find_asset(p != nullptr ? p->id : 0);
	lua_pushnumber(L, a != nullptr ? (lua_Number) a->h : 0);
	return 1;
}

static int pm_setFilter(lua_State *L)
{
	FrAsset *a = proxy_asset(L);
	const char *mn = luaL_checkstring(L, 2);
	const char *mg = luaL_checkstring(L, 3);
	if (a != nullptr)
	{
		a->filter_min = mn;
		a->filter_mag = mg;
	}
	return 0;
}

static int pm_getFilter(lua_State *L)
{
	FrAsset *a = proxy_asset(L);
	lua_pushstring(L, a != nullptr ? a->filter_min.c_str() : "linear");
	lua_pushstring(L, a != nullptr ? a->filter_mag.c_str() : "linear");
	return 2;
}

static int pm_getType(lua_State *L)
{
	FrProxy *p = proxy_arg(L);
	const char *t = "unknown";
	if (p != nullptr)
	{
		switch (p->kind)
		{
		case FR_ASSET_IMAGE: t = "Image"; break;
		case FR_ASSET_CANVAS: t = "Canvas"; break;
		case FR_ASSET_QUAD: t = "Quad"; break;
		case FR_ASSET_FONT: t = "Font"; break;
		}
	}
	lua_pushstring(L, t);
	return 1;
}

// 方法表（__index 首查）：同名方法按 kind 分派（getWidth 在 image=几何、
// font=排版宽度——pm_getWidth 内分派）。
static const struct { const char *name; lua_CFunction fn; } k_proxy_methods[] =
{
	{"getWidth", pm_getWidth},
	{"getHeight", pm_getHeight},
	{"getDimensions", pm_getDimensions},
	{"getPixelWidth", pm_getPixelWidth},
	{"getPixelHeight", pm_getPixelHeight},
	{"setFilter", pm_setFilter},
	{"getFilter", pm_getFilter},
	{"getType", pm_getType},
	{"getLineHeight", pm_getLineHeight},
	{"getAscent", pm_getAscent},
	{"getDescent", pm_getDescent},
	{"getBaseline", pm_getBaseline},
	{nullptr, nullptr}
};

static int proxy_index(lua_State *L)
{
	FrProxy *p = (FrProxy *) lua_touserdata(L, 1);
	const char *k = luaL_checkstring(L, 2);
	for (auto *m = k_proxy_methods; m->name != nullptr; m++)
	{
		if (strcmp(k, m->name) == 0)
		{
			lua_pushcfunction(L, m->fn);
			return 1;
		}
	}
	if (strcmp(k, "id") == 0)
	{
		lua_pushinteger(L, (lua_Integer) p->id);
		return 1;
	}
	if (strcmp(k, "kind") == 0)
	{
		lua_pushinteger(L, p->kind);
		return 1;
	}
	// B9 探针：游戏访问未支持的字段/方法——兼容缺口现场可见（stderr 每
	// 键一次），配合冒烟迭代收窄代理面。
	static std::vector<std::string> seen;
	std::string key(k);
	if (std::find(seen.begin(), seen.end(), key) == seen.end())
	{
		seen.push_back(key);
		std::fprintf(stderr, "fr: 代理字段未支持: %s（id=%u kind=%d）\n",
		             k, (unsigned) (p != nullptr ? p->id : 0),
		             p != nullptr ? p->kind : -1);
	}
	return luaL_error(L, "fr.asset proxy: 未知字段 \"%s\"（方法族/可用 id/kind）", k);
}

static int proxy_tostring(lua_State *L)
{
	FrProxy *p = (FrProxy *) lua_touserdata(L, 1);
	lua_pushfstring(L, "fr.asset(id=%u, kind=%s)", p->id, kind_name(p->kind));
	return 1;
}

static void push_proxy(lua_State *L, const FrAsset &a)
{
	if (luaL_newmetatable(L, "fr_asset_proxy"))
	{
		lua_pushcfunction(L, proxy_index);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, proxy_tostring);
		lua_setfield(L, -2, "__tostring");
	}
	lua_pop(L, 1);

	FrProxy *p = (FrProxy *) lua_newuserdata(L, sizeof(FrProxy));
	p->id = a.id;
	p->kind = a.kind;
	luaL_getmetatable(L, "fr_asset_proxy");
	lua_setmetatable(L, -2);
}

bool fr_isasset(lua_State *L, int idx)
{
	if (lua_type(L, idx) != LUA_TUSERDATA)
		return false;
	if (lua_getmetatable(L, idx) == 0)
		return false;
	luaL_getmetatable(L, "fr_asset_proxy");
	bool same = lua_rawequal(L, -1, -2) != 0;
	lua_pop(L, 2);
	return same;
}

love::uint32 fr_assetid(lua_State *L, int idx)
{
	FrProxy *p = (FrProxy *) lua_touserdata(L, idx);
	return p != nullptr ? p->id : 0;
}

// B8 T1 字体状态（w_setFont 挂钩记录 / w_print 未传 font 时回退）。
static love::uint32 s_current_font = 0;

void fr_set_current_font(love::uint32 id) { s_current_font = id; }
love::uint32 fr_current_font() { return s_current_font; }

static love::uint32 register_asset(const FrAsset &a)
{
	// B9 资源爆炸对策①——不可变资产去重：image/font/quad 内容由参数全决
	//（字节/几何），同参重建 = 同一资产（游戏侧多 cache 重复创建的兜底，
	// 典型：font.lua 多处 M.get(size) 各自建、tileset 多处 newImage 同路径）
	//——复用既有 id，不重复产行。**canvas 永不去重**：可变渲染目标，同尺寸
	// 内容独立（world_map fog/minimap 两块同参 canvas 是两个资产）。
	if (a.kind != FR_ASSET_CANVAS)
	{
		for (const auto &e : assets())
		{
			if (e.kind != a.kind || e.w != a.w || e.h != a.h)
				continue;
			if (e.kind == FR_ASSET_FONT && e.size != a.size)
				continue;
			if (e.kind == FR_ASSET_QUAD
			    && (e.qx != a.qx || e.qy != a.qy || e.qw != a.qw || e.qh != a.qh
			        || e.qsw != a.qsw || e.qsh != a.qsh))
				continue;
			if (e.data_hex != a.data_hex)
				continue;
			return e.id;
		}
	}
	assets().push_back(a);
	asset_rows().push_back(render_asset_row(a));
	// B9 资源爆炸对策②——观测护栏（不设限，一次性告警）：真实游戏资产面
	// 超出 demo 量级时现场可见（流体积 = 登记字节总量主导）。
	static love::uint64 total_bytes = 0;
	static bool warned = false;
	total_bytes += a.data_hex.size() / 2;
	if (!warned && (assets().size() > 256 || total_bytes > 16ull * 1024 * 1024))
	{
		std::fprintf(stderr, "fr: 资产量级告警——登记 %zu 项 / %llu 字节"
			"（会话体积主导项，检查去重/生命周期）\n",
			assets().size(), (unsigned long long) total_bytes);
		warned = true;
	}
	return a.id;
}

// 物化侧查表（fr_materialize 用）：登记资产的只读拷贝视图。
bool fr_lookup_asset(love::uint32 id, FrAssetInfo &out)
{
	for (const FrAsset &a : assets())
	{
		if (a.id != id)
			continue;
		out.kind = a.kind;
		out.w = a.w;
		out.h = a.h;
		out.size = a.size;
		out.data_hex = a.data_hex;
		return true;
	}
	return false;
}

// 流内资产登记（拓扑 B 离线回放）：id 取流行，不重编号；只入登记表
//（物化器消费），不入 asset_rows（离线不回产会话文本）。
bool fr_register_stream_asset(int kind, love::uint32 id, int w, int h, float size,
                              const float *quad, const std::string &data_hex)
{
	if (id == 0)
		return false; // 0 = 空资产哨兵，不可登记

	FrAsset a;
	a.kind = kind;
	a.id = id;
	a.w = w;
	a.h = h;
	a.size = size;
	if (quad != nullptr)
	{
		a.qx = quad[0]; a.qy = quad[1]; a.qw = quad[2];
		a.qh = quad[3]; a.qsw = quad[4]; a.qsh = quad[5];
	}
	a.data_hex = data_hex;
	assets().push_back(a);
	return true;
}

// ------------------------------------------------------------------
// 语义替代四件套（w_new* 挂钩转入）
// ------------------------------------------------------------------

int w_frNewImage(lua_State *L)
{
	FrAsset a;
	a.kind = FR_ASSET_IMAGE;

	if (lua_type(L, 1) == LUA_TSTRING)
	{
		// 文件路径：读真实字节登记。宽高 demo 期无真解码登记 0（占位渲染
		// 不可见；真解码归后续批次——hex 是编码后文件字节，非像素域）。
		const char *filename = luaL_checkstring(L, 1);
		love::filesystem::Filesystem *fs =
			love::Module::getInstance<love::filesystem::Filesystem>(love::Module::M_FILESYSTEM);
		if (fs == nullptr)
			return luaL_error(L, "fr.newImage: filesystem 未初始化");

		love::filesystem::FileData *data = nullptr;
		love::luax_catchexcept(L, [&]() { data = fs->read(filename); });

		a.data_hex = hex_encode(data->getData(), data->getSize());
		data->release();
	}
	else
	{
		// ImageData 路径（B4 阶段三真字节口径）：像素字节原样 hex 登记
		//（协议口径 RGBA8 像素域，物化侧按 newImageData(w,h,RGBA8,bytes)
		// 同链重建，见 fr_materialize.cpp）。
		love::image::ImageData *d = love::luax_checktype<love::image::ImageData>(L, 1);
		a.w = d->getWidth();
		a.h = d->getHeight();
		a.data_hex = hex_encode(d->getData(), d->getSize());
	}

	a.id = next_asset_id();
	a.id = register_asset(a); // B9 去重可复用既有 id（不可变资产）
	push_proxy(L, a);
	return 1;
}

int w_frNewFont(lua_State *L)
{
	// B9 形态扩面（真实游戏 font.lua 主路径）：
	//   newFont(size)              → 内嵌 Vera（demo 基线，原语义）
	//   newFont(path [, size])     → 文件真字节登记（中文 TTF 数 MB，去重兜底）
	//   newFont(FileData [, size]) → 同上（系统字体 io.open 读字节形态）
	//   Rasterizer 形态            → 边界（Lua error——如实阻断，无字节可登记）
	// size 缺省 12（love 同缺省）。
	FrAsset a;
	a.kind = FR_ASSET_FONT;
	int t = lua_type(L, 1);

	if (t == LUA_TNUMBER)
	{
		a.size = (float) luaL_checknumber(L, 1);
		a.data_hex = hex_encode(love::font::Vera_ttf, love::font::Vera_ttf_len);
	}
	else if (t == LUA_TSTRING || luax_istype(L, 1, love::filesystem::FileData::type))
	{
		const love::filesystem::FileData *bytes = nullptr;
		love::filesystem::FileData *owned = nullptr;
		if (t == LUA_TSTRING)
		{
			const char *path = luaL_checkstring(L, 1);
			love::filesystem::Filesystem *fs =
				love::Module::getInstance<love::filesystem::Filesystem>(love::Module::M_FILESYSTEM);
			if (fs == nullptr)
				return luaL_error(L, "fr.newFont: filesystem 未初始化");
			love::luax_catchexcept(L, [&]() { owned = fs->read(path); });
			bytes = owned;
		}
		else
			bytes = love::luax_checktype<love::filesystem::FileData>(L, 1); // 栈持有
		a.size = (float) luaL_optnumber(L, 2, 12.0);
		a.data_hex = hex_encode(bytes->getData(), bytes->getSize());
		if (owned != nullptr)
			owned->release();
	}
	else
	{
		return luaL_error(L, "fr.newFont: Rasterizer 形态未支持（语义边界——无字节可登记）");
	}

	a.id = next_asset_id();
	love::uint32 id = register_asset(a); // 去重可复用既有 id
	a.id = id;
	push_proxy(L, a);
	return 1;
}

int w_frNewCanvas(lua_State *L)
{
	FrAsset a;
	a.kind = FR_ASSET_CANVAS;
	a.w = (int) luaL_checkinteger(L, 1);
	a.h = (int) luaL_checkinteger(L, 2);
	// 不建真 Canvas（文件头 Canvas 边界：回读 rig 走 _frNewCanvasReal）。
	a.id = next_asset_id();
	a.id = register_asset(a); // canvas 不去重（可变目标），统一同口
	push_proxy(L, a);
	return 1;
}

int w_frNewQuad(lua_State *L)
{
	FrAsset a;
	a.kind = FR_ASSET_QUAD;
	a.qx = (float) luaL_checknumber(L, 1);
	a.qy = (float) luaL_checknumber(L, 2);
	a.qw = (float) luaL_checknumber(L, 3);
	a.qh = (float) luaL_checknumber(L, 4);
	a.qsw = (float) luaL_checknumber(L, 5);
	a.qsh = (float) luaL_checknumber(L, 6);
	a.id = next_asset_id();
	a.id = register_asset(a); // B9 去重可复用既有 id（纯几何资产）
	push_proxy(L, a);
	return 1;
}

// ------------------------------------------------------------------
// 回放（present 钩 / _frReplay 入口 / 拓扑 B 离线回放共用行执行）
// ------------------------------------------------------------------

// 占位伪色映射（id → 确定性颜色；1→白 2→青 其它→灰，见 fr_capture.h）。
static love::Colorf pseudocolor(love::uint32 id)
{
	switch (id)
	{
	case 1: return love::Colorf(1.0f, 1.0f, 1.0f, 1.0f);
	case 2: return love::Colorf(0.0f, 1.0f, 1.0f, 1.0f);
	default: return love::Colorf(0.5f, 0.5f, 0.5f, 1.0f);
	}
}

static void lookup_asset_size(love::uint32 id, int &w, int &h)
{
	w = 0;
	h = 0;
	for (const FrAsset &a : assets())
	{
		if (a.id == id && (a.kind == FR_ASSET_IMAGE || a.kind == FR_ASSET_CANVAS))
		{
			w = a.w;
			h = a.h;
			return;
		}
	}
}

int fr_asset_kind_of(love::uint32 id)
{
	for (const FrAsset &a : assets())
		if (a.id == id)
			return a.kind;
	return 0;
}

// 单行执行：.fr 行 = 空格分隔 token，首 token 为 op 名（golden/l0_session.fr
// 同构）。tee₂（capture_replay）与拓扑 B 离线回放共用本入口——同一物化器、
// 同一真渲染路径（B4-2 验收线第二分句的同构关键）。
// B8 T3：回放会话基线——rt 0 的"屏幕"在回放语境 = 会话起点目标。两个回放
// 驱动（capture_replay / 拓扑 B 离线循环）进出配对设置/清零；循环外零效。
// RenderTargets 持裸指针：基线画布由调用方（Lua 验证帧/离线局部）保活，
// 生命周期覆盖回放循环（单线程，无悬挂窗口）。
static love::graphics::Graphics::RenderTargets g_replay_baseline;
static bool g_replay_baseline_canvas = false;

void capture_exec_row(love::graphics::Graphics *gfx, const std::string &row)
{
	if (gfx == nullptr)
		return;

	if (row.compare(0, 6, "clear ") == 0)
	{
		int r = 0, g = 0, b = 0, a = 255;
		if (sscanf(row.c_str(), "clear %d %d %d %d", &r, &g, &b, &a) == 4)
		{
			love::Colorf cf(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
			// stencil/depth 按 w_clear 的缺省口径（0 / 1.0），见 fr_capture.h 边界。
			gfx->clear(love::graphics::OptionalColorf(cf), love::OptionalInt(0), love::OptionalDouble(1.0));
		}
	}
	else if (row.compare(0, 5, "draw ") == 0)
	{
		// 真纹理物化优先（B4 阶段三）：登记字节 → Image（fr_materialize，
		// 会话内缓存），行内颜色 = 捕获时 setColor 状态（纹理色 × 画色）。
		// quad≠0 仍走占位（Quad 视口裁剪归后续批次）；未登记/物化失败
		// 回退伪色矩形（伪色按 id 确定性映射）。
		unsigned tex = 0, quad = 0;
		float x = 0, y = 0, rot = 0, sx = 0, sy = 0, ox = 0, oy = 0;
		int r = 0, g = 0, b = 0, a = 255;
		if (sscanf(row.c_str(), "draw %u %u %f %f %f %f %f %f %f %d %d %d %d",
		           &tex, &quad, &x, &y, &rot, &sx, &sy, &ox, &oy, &r, &g, &b, &a) == 13)
		{
			// B9 canvas 合成：draw 引用 canvas 资产（fog/minimap 贴回屏幕）
			//——物化为回放目标 Canvas（与 rt 行同一缓存：rt 段画进去、这里
			// 贴出来 = cmd.tbl rt 行注记"canvas 间合成=draw 引用"的兑现）。
			love::graphics::Image *img = nullptr;
			love::graphics::Canvas *cv = nullptr;
			if (quad == 0)
			{
				if (fr_asset_kind_of(tex) == FR_ASSET_CANVAS)
					cv = fr_materialize_canvas(gfx, tex);
				else
					img = fr_materialize_image(gfx, tex);
			}
			love::graphics::Drawable *drawable =
				img != nullptr ? (love::graphics::Drawable *) img
				: cv != nullptr ? (love::graphics::Drawable *) cv : nullptr;
			if (drawable != nullptr)
			{
				// 裸参数矩阵重建（luax_checkstandardtransform 的
				// Matrix4(x,y,a,sx,sy,ox,oy,kx,ky) 同构；x/y 已是捕获时
				// 结算的世界系，回放变换栈恒 origin，无二次变换）。
				love::Matrix4 m(x, y, rot, sx, sy, ox, oy, 0.0f, 0.0f);
				gfx->setColor(love::Colorf(r / 255.0f, g / 255.0f,
				                           b / 255.0f, a / 255.0f));
				gfx->draw(drawable, m);
			}
			else
			{
				gfx->setColor(pseudocolor(tex));
				int w = 0, h = 0;
				lookup_asset_size(tex, w, h);
				if (w > 0 && h > 0)
					gfx->rectangle(love::graphics::Graphics::DRAW_FILL, x, y, (float) w, (float) h);
			}
		}
	}
	else if (row.compare(0, 6, "print ") == 0)
	{
		// 行解析（esc 反转义）：`print <font> "<escaped>" x y rot sx sy
		// rgba`——手写引号扫描（sscanf %[^"] 会被转义 \" 截断），
		// 引号内转义序列经 unesc 展开为原始字节。
		unsigned font = 0;
		float x = 0, y = 0, rot = 0, sx = 0, sy = 0;
		int r = 0, g = 0, b = 0, a = 255;
		std::string text;
		bool parsed = false;

		const char *p = row.c_str() + 6;
		char *endp = nullptr;
		font = (unsigned) strtoul(p, &endp, 10);
		if (endp > p && endp[0] == ' ' && endp[1] == '"')
		{
			const char *q = endp + 2;
			std::string raw;
			while (*q != '\0')
			{
				if (*q == '\\' && q[1] != '\0')
				{
					raw += q[0];
					raw += q[1];
					q += 2;
					continue;
				}
				if (*q == '"')
					break;
				raw += *q++;
			}
			if (*q == '"')
			{
				text = unesc(raw);
				parsed = sscanf(q + 1, " %f %f %f %f %f %d %d %d %d",
				                &x, &y, &rot, &sx, &sy, &r, &g, &b, &a) == 9;
			}
		}

		if (parsed)
		{
			// 真字形优先（登记 Vera 字节 → Rasterizer → Font）；失败回退
			// 伪色小矩形（8x16 每字符）。行内颜色 = 捕获时 setColor 状态
			//（多段 ColoredString 已拼单串，段色不存——demo 单段）。
			love::graphics::Font *fnt = fr_materialize_font(gfx, font);
			if (fnt != nullptr)
			{
				love::Colorf cf(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
				gfx->setColor(cf);
				std::vector<love::graphics::Font::ColoredString> str;
				love::graphics::Font::ColoredString cs;
				cs.str = text;
				cs.color = cf;
				str.push_back(cs);
				love::Matrix4 m(x, y, rot, sx, sy, 0.0f, 0.0f, 0.0f, 0.0f);
				gfx->print(str, fnt, m);
			}
			else
			{
				gfx->setColor(pseudocolor(font));
				gfx->rectangle(love::graphics::Graphics::DRAW_FILL, x, y,
				               8.0f * (float) text.size(), 16.0f);
			}
		}
	}
	else if (row.compare(0, 5, "rect ") == 0)
	{
		unsigned mode = 0;
		float x = 0, y = 0, w = 0, h = 0, rx = 0, ry = 0, width = 1.0f;
		int r = 0, g = 0, b = 0, a = 255;
		if (sscanf(row.c_str(), "rect %u %f %f %f %f %f %f %f %d %d %d %d",
		           &mode, &x, &y, &w, &h, &rx, &ry, &width, &r, &g, &b, &a) == 12)
		{
			// cmd.tbl mode 0=fill 1=line；love 枚举 DRAW_LINE=0 DRAW_FILL=1——换算。
			love::graphics::Graphics::DrawMode m =
				(mode == 0) ? love::graphics::Graphics::DRAW_FILL
				            : love::graphics::Graphics::DRAW_LINE;
			gfx->setColor(love::Colorf(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f));
			gfx->setLineWidth(width); // B8 T1：行内线宽应用（live/离线状态对齐）
			if (rx > 0.0f || ry > 0.0f)
				gfx->rectangle(m, x, y, w, h, rx, ry); // 圆角 points 缺省（行内无此字段）
			else
				gfx->rectangle(m, x, y, w, h);
		}
	}
	else if (row.compare(0, 5, "line ") == 0)
	{
		// `line <n> <x1> <y1> ... <width> <rgba>`——逐 token 数值化（可变点数）。
		std::vector<float> vals;
		{
			std::istringstream iss(row);
			std::string tag;
			iss >> tag;
			float v;
			while (iss >> v) vals.push_back(v);
		}
		if (vals.size() >= 7) // n + 2n coords + width + 4 color = 3n+5 ≥ 11... but n itself is vals[0]
		{
			unsigned n = (unsigned) vals[0];
			if (n >= 1 && vals.size() >= (size_t) (1 + n * 2 + 5))
			{
				float width = vals[1 + n * 2];
				int r = (int) vals[1 + n * 2 + 1];
				int g = (int) vals[1 + n * 2 + 2];
				int b = (int) vals[1 + n * 2 + 3];
				int a = (int) vals[1 + n * 2 + 4];
				std::vector<love::Vector2> pts(n);
				for (unsigned i = 0; i < n; i++)
				{
					pts[i].x = vals[1 + i * 2];
					pts[i].y = vals[1 + i * 2 + 1];
				}
				gfx->setColor(love::Colorf(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f));
				gfx->setLineWidth(width);
				gfx->polyline(pts.data(), n);
			}
		}
	}
	else if (row.compare(0, 7, "circle ") == 0)
	{
		unsigned mode = 0, seg = 32;
		float x = 0, y = 0, r = 0;
		int cr = 0, cg = 0, cb = 0, ca = 255;
		if (sscanf(row.c_str(), "circle %u %f %f %f %u %d %d %d %d",
		           &mode, &x, &y, &r, &seg, &cr, &cg, &cb, &ca) == 9)
		{
			gfx->setColor(love::Colorf(cr / 255.0f, cg / 255.0f, cb / 255.0f, ca / 255.0f));
			gfx->circle(mode == 0 ? love::graphics::Graphics::DRAW_FILL
			                      : love::graphics::Graphics::DRAW_LINE,
			            x, y, r, (int) seg);
		}
	}
	else if (row.compare(0, 5, "poly ") == 0)
	{
		std::vector<float> vals;
		{
			std::istringstream iss(row);
			std::string tag;
			iss >> tag;
			float v;
			while (iss >> v) vals.push_back(v);
		}
		if (vals.size() >= 6) // mode + n + 2n coords + 4 color
		{
			unsigned mode = (unsigned) vals[0];
			unsigned n = (unsigned) vals[1];
			if (n >= 3 && vals.size() >= (size_t) (2 + n * 2 + 4))
			{
				int r = (int) vals[2 + n * 2];
				int g = (int) vals[2 + n * 2 + 1];
				int b = (int) vals[2 + n * 2 + 2];
				int a = (int) vals[2 + n * 2 + 3];
				std::vector<love::Vector2> pts(n + 1); // 闭合
				for (unsigned i = 0; i < n; i++)
				{
					pts[i].x = vals[2 + i * 2];
					pts[i].y = vals[2 + i * 2 + 1];
				}
				pts[n] = pts[0];
				gfx->setColor(love::Colorf(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f));
				gfx->polygon(mode == 0 ? love::graphics::Graphics::DRAW_FILL
				                       : love::graphics::Graphics::DRAW_LINE,
				             pts.data(), n + 1);
			}
		}
	}
	else if (row.compare(0, 4, "arc ") == 0)
	{
		unsigned mode = 0, at = 0, seg = 16;
		float x = 0, y = 0, r = 0, a1 = 0, a2 = 0;
		int cr = 0, cg = 0, cb = 0, ca = 255;
		if (sscanf(row.c_str(), "arc %u %u %f %f %f %f %f %u %d %d %d %d",
		           &mode, &at, &x, &y, &r, &a1, &a2, &seg, &cr, &cg, &cb, &ca) == 12)
		{
			gfx->setColor(love::Colorf(cr / 255.0f, cg / 255.0f, cb / 255.0f, ca / 255.0f));
			gfx->arc(mode == 0 ? love::graphics::Graphics::DRAW_FILL
			                   : love::graphics::Graphics::DRAW_LINE,
			         (at == 1) ? love::graphics::Graphics::ARC_OPEN
			         : (at == 2) ? love::graphics::Graphics::ARC_CLOSED
			                     : love::graphics::Graphics::ARC_PIE,
			         x, y, r, a1, a2, (int) seg);
		}
	}
	else if (row.compare(0, 7, "points ") == 0)
	{
		std::vector<float> vals;
		{
			std::istringstream iss(row);
			std::string tag;
			iss >> tag;
			float v;
			while (iss >> v) vals.push_back(v);
		}
		if (vals.size() >= 7) // n + 2n coords + size + 4 color
		{
			unsigned n = (unsigned) vals[0];
			if (n >= 1 && vals.size() >= (size_t) (1 + n * 2 + 5))
			{
				float size = vals[1 + n * 2];
				int r = (int) vals[1 + n * 2 + 1];
				int g = (int) vals[1 + n * 2 + 2];
				int b = (int) vals[1 + n * 2 + 3];
				int a = (int) vals[1 + n * 2 + 4];
				gfx->setColor(love::Colorf(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f));
				gfx->setPointSize(size);
				std::vector<love::Vector2> pts(n);
				for (unsigned i = 0; i < n; i++)
				{
					pts[i].x = vals[1 + i * 2];
					pts[i].y = vals[1 + i * 2 + 1];
				}
				std::vector<love::Colorf> cols(n, love::Colorf(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f));
				gfx->points(pts.data(), cols.data(), n);
			}
		}
	}
	else if (row.compare(0, 3, "rt ") == 0)
	{
		// B8 T3：目标切换（0=屏幕）。回放侧：真 Canvas 由物化器建。
		// rt 0 的"屏幕"在回放语境 = 会话基线目标（present 真屏幕 / tee₂
		// 验证 Canvas / 拓扑 B 离线 Canvas）——盲切默认缓冲会把基线画布
		// 上的帧内容劫持到真屏幕（B8 T3 实测教训）。
		unsigned target = 0;
		if (sscanf(row.c_str(), "rt %u", &target) == 1)
		{
			if (target == 0)
			{
				if (g_replay_baseline_canvas)
					gfx->setCanvas(g_replay_baseline);
				else
					gfx->setCanvas();
				return;
			}
			love::graphics::Canvas *cv = fr_materialize_canvas(gfx, target);
			if (cv != nullptr)
			{
				love::graphics::Graphics::RenderTarget rtArg(cv);
				gfx->setCanvas(rtArg, 0);
			}
			else
				std::fprintf(stderr, "fr: rt %u 物化失败，跳过目标切换\n", target);
		}
	}
	else if (row.compare(0, 8, "scissor ") == 0)
	{
		float x = 0, y = 0, w = 0, h = 0;
		if (sscanf(row.c_str(), "scissor %f %f %f %f", &x, &y, &w, &h) == 4)
		{
			if (x < 0 || y < 0) // -1 哨兵 = off
				gfx->setScissor();
			else
			{
				love::Rect r;
				r.x = (int) x; r.y = (int) y; r.w = (int) w; r.h = (int) h;
				gfx->setScissor(r);
			}
		}
	}
	else if (row.compare(0, 4, "err ") == 0)
	{
		// err 行（Shader 边界等）：执行侧 no-op（登记性行，回放不消费）。
	}
}

void capture_replay(love::graphics::Graphics *gfx)
{
	if (gfx == nullptr)
		return;

	love::graphics::Graphics::RenderTargets saved = gfx->getCanvas();
	g_replay_baseline = saved;
	g_replay_baseline_canvas = !saved.colors.empty();
	for (const std::string &row : rows())
		capture_exec_row(gfx, row);
	// 收尾回基线：rt 3 中途留在物化 Canvas 的帧也归位（基线=屏幕时与
	// setCanvas() 同义；present 钩路径天然如此）。
	if (g_replay_baseline_canvas)
		gfx->setCanvas(saved);
	else
		gfx->setCanvas();
	g_replay_baseline_canvas = false;
}

// B9：present 上屏增量重放——游标记已重放行，每帧只执行新增（行缓冲仍
// 全会话累积，tee₁ 不受影响）。语义等价：真执行同样不清历史帧，本帧行
// 含 clear 时自然覆盖。**性能**：全量重放成本 O(会话行数)/帧——demo 25
// 行/帧无感，真实游戏 2500+ tile draw 行/帧 → 二次方爆炸（newgame 实测
// 每帧 11s+）。验证 Canvas（_frReplay，空白画布）仍走全量 capture_replay。
void capture_replay_delta(love::graphics::Graphics *gfx)
{
	static size_t replayed = 0;
	if (gfx == nullptr)
		return;

	std::vector<std::string> &rs = rows();
	if (rs.size() <= replayed)
		return; // 本帧无新增行（如纯事件帧）

	love::graphics::Graphics::RenderTargets saved = gfx->getCanvas();
	g_replay_baseline = saved;
	g_replay_baseline_canvas = !saved.colors.empty();
	for (size_t i = replayed; i < rs.size(); i++)
		capture_exec_row(gfx, rs[i]);
	replayed = rs.size();
	if (g_replay_baseline_canvas)
		gfx->setCanvas(saved);
	else
		gfx->setCanvas();
	g_replay_baseline_canvas = false;
}

// ------------------------------------------------------------------
// Lua 出口
// ------------------------------------------------------------------

std::vector<std::string> capture_take_rows()
{
	return rows();
}

// B8 T4：捕获期 err 行（累积拷贝；demo 侧切 per-frame delta——与 out 行
// take_delta 同构，帧界由 Lua 按已取行数切）。
std::vector<std::string> fr_take_err_rows()
{
	return err_rows();
}

std::vector<std::string> fr_take_asset_rows()
{
	return asset_rows();
}

static int push_string_array(lua_State *L, const std::vector<std::string> &rs)
{
	lua_createtable(L, (int) rs.size(), 0);
	for (size_t i = 0; i < rs.size(); i++)
	{
		lua_pushlstring(L, rs[i].data(), rs[i].size());
		lua_rawseti(L, -2, (int) i + 1);
	}
	return 1;
}

int w_frTakeSession(lua_State *L)
{
	return push_string_array(L, rows());
}

int w_frTakeErrRows(lua_State *L)
{
	return push_string_array(L, err_rows());
}

// B8 T3：观察侧围栏出口（demo 验证帧 rig 机器行不进内核流；配对调用，
// 计数嵌套安全，越界 resume 为 no-op）。
int w_frCaptureSuspend(lua_State *L)
{
	capture_suspend();
	return 0;
}

int w_frCaptureResume(lua_State *L)
{
	capture_resume();
	return 0;
}

// B10：显式补帧口（demo 验证帧不走 present——rig 自己补调，帧块序完整；
// 第三方游戏 present 天然每帧，无此需求）。
int w_frSessionFrame(lua_State *L)
{
	love::graphics::Graphics *gfx =
		love::Module::getInstance<love::graphics::Graphics>(love::Module::M_GRAPHICS);
	fr_session_frame(gfx);
	return 0;
}

int w_frTakeAssets(lua_State *L)
{
	return push_string_array(L, asset_rows());
}

int w_frReplay(lua_State *)
{
	love::graphics::Graphics *gfx =
		love::Module::getInstance<love::graphics::Graphics>(love::Module::M_GRAPHICS);
	capture_replay(gfx);
	return 0;
}

int w_frNewCanvasReal(lua_State *L)
{
	love::graphics::Graphics *gfx =
		love::Module::getInstance<love::graphics::Graphics>(love::Module::M_GRAPHICS);
	int w = (int) luaL_checkinteger(L, 1);
	int h = (int) luaL_checkinteger(L, 2);

	// 真 Canvas（开关无关）：验证帧回读 rig 专用。dpiScale=1 定死像素域 =
	// 逻辑域（默认 getScreenDPIScale() 会随窗口 DPI 漂移，回读坐标失准）。
	love::graphics::Canvas::Settings settings;
	settings.width = w;
	settings.height = h;
	settings.dpiScale = 1.0f;

	love::graphics::Canvas *canvas = nullptr;
	love::luax_catchexcept(L, [&]() { canvas = gfx->newCanvas(settings); });

	love::luax_pushtype(L, canvas);
	canvas->release();
	return 1;
}

// ------------------------------------------------------------------
// 拓扑 B 离线回放 + D_CRC 采集（B4 阶段三 Task 3）
// ------------------------------------------------------------------

// CRC32（l0/walker.lua 同构：查表 0xEDB88320、init/xorout 0xFFFFFFFF、
// reflected；空输入 = 0）。live 验证帧（w_frImageDataCRC）与离线回放
// 共用一次性外壳 fr_crc32_bytes——对拍锚的"同采集函数"；增量入口
// fr_crc32_update 供 fr_statediff 分段混入（B6 堆 CRC 与 D_CRC 同一
// CRC32 机，勿改参数语义）。
static love::uint32 *crc32_table()
{
	static love::uint32 *table = []()
	{
		static love::uint32 tbl[256];
		for (int i = 0; i < 256; i++)
		{
			love::uint32 c = (love::uint32) i;
			for (int k = 0; k < 8; k++)
				c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
			tbl[i] = c;
		}
		return tbl;
	}();
	return table;
}

love::uint32 fr_crc32_update(love::uint32 state, const void *data, size_t n)
{
	const love::uint32 *table = crc32_table();
	const love::uint8 *p = (const love::uint8 *) data;
	love::uint32 crc = state;
	for (size_t i = 0; i < n; i++)
		crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xFFu];
	return crc;
}

love::uint32 fr_crc32_bytes(const void *data, size_t n)
{
	return fr_crc32_update(0xFFFFFFFFu, data, n) ^ 0xFFFFFFFFu;
}

// 离线会话：头几何 + 每帧 out 行（资产在解析期直接入登记表）。
struct OfflineSession
{
	int width = 640, height = 480;
	std::vector<std::vector<std::string>> frames;
	std::vector<double> dts; // B11 FR_PLAY：按录制 dt 节奏播放（默认 1/60）
};

static bool hex_valid(const std::string &s)
{
	if (s.empty())
		return true; // 空 = "-" 占位
	for (char c : s)
	{
		bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		if (!ok)
			return false;
	}
	return true;
}

// 资产行（文本形态）→ 登记。"asset image <id> <w> <h> <hex|->" /
// "asset canvas <id> <w> <h>" / "asset quad <id> %.6f×6" /
// "asset font <id> <size> <hex|->"（render_asset_row 同构）。
static int offline_register_asset_text(const std::string &row, int lineno)
{
	unsigned id = 0;
	int w = 0, h = 0, pos = 0;
	float size = 0.0f;
	float q[6] = {0, 0, 0, 0, 0, 0};
	std::string hex;

	if (sscanf(row.c_str(), "asset image %u %d %d %n", &id, &w, &h, &pos) == 3)
	{
		hex = row.substr((size_t) pos);
		if (hex == "-")
			hex.clear();
		if (!hex_valid(hex))
			return 0;
		return fr_register_stream_asset(FR_ASSET_IMAGE, id, w, h, 0.0f, nullptr, hex) ? 1 : 0;
	}
	if (sscanf(row.c_str(), "asset canvas %u %d %d", &id, &w, &h) == 3)
		return fr_register_stream_asset(FR_ASSET_CANVAS, id, w, h, 0.0f, nullptr, "") ? 1 : 0;
	if (sscanf(row.c_str(), "asset quad %u %f %f %f %f %f %f",
	           &id, &q[0], &q[1], &q[2], &q[3], &q[4], &q[5]) == 7)
		return fr_register_stream_asset(FR_ASSET_QUAD, id, 0, 0, 0.0f, q, "") ? 1 : 0;
	if (sscanf(row.c_str(), "asset font %u %f %n", &id, &size, &pos) == 2)
	{
		hex = row.substr((size_t) pos);
		if (hex == "-")
			hex.clear();
		if (!hex_valid(hex))
			return 0;
		return fr_register_stream_asset(FR_ASSET_FONT, id, 0, 0, size, nullptr, hex) ? 1 : 0;
	}
	(void) lineno;
	return 0;
}

// 文本（.fr canonical）→ 会话。结构口径对齐 tools/fr_session.parse_text
//（行号 1 起报错；行必须位于 frame 块内；帧号连续）。out 行只收本层执行
// 域（clear/draw/print/rect），dt/cmnt 跳过——fr_check 已保证 canonical。
static int offline_parse_text(const std::string &buf, OfflineSession &s)
{
	if (buf.empty() || buf.back() != '\n')
	{
		std::fprintf(stderr, "fr-offline: 第 1 行：文本必须以单个 LF 结尾\n");
		return 3;
	}

	std::vector<std::string> lines;
	size_t start = 0;
	while (start < buf.size())
	{
		size_t end = buf.find('\n', start);
		lines.push_back(buf.substr(start, end - start));
		start = end + 1;
	}

	int w = 0, h = 0;
	if (lines.empty() || sscanf(lines[0].c_str(), "frb1 %d %d", &w, &h) != 2
	    || w <= 0 || h <= 0)
	{
		std::fprintf(stderr, "fr-offline: 第 1 行：会话头必须为 'frb1 w h sha loglvl seed'\n");
		return 3;
	}
	s.width = w;
	s.height = h;

	bool in_frame = false;
	for (size_t i = 1; i < lines.size(); i++)
	{
		const std::string &ln = lines[i];
		int no = (int) i + 1;
		if (ln.empty() || ln.find_first_not_of(" \t") == std::string::npos)
		{
			std::fprintf(stderr, "fr-offline: 第 %d 行：空行禁入 canonical\n", no);
			return 3;
		}
		if (ln.compare(0, 5, "cmnt ") == 0)
		{
			in_frame = false;
			continue;
		}
		if (ln.compare(0, 6, "frame ") == 0)
		{
			unsigned n = 0;
			if (sscanf(ln.c_str(), "frame %u", &n) != 1 || n != s.frames.size())
			{
				std::fprintf(stderr, "fr-offline: 第 %d 行：帧号必须为 %d（连续）\n",
				             no, (int) s.frames.size());
				return 3;
			}
			s.frames.push_back(std::vector<std::string>());
			in_frame = true;
			continue;
		}
		if (!in_frame)
		{
			std::fprintf(stderr, "fr-offline: 第 %d 行：行必须位于 frame 块内\n", no);
			return 3;
		}
		if (ln.compare(0, 3, "dt ") == 0)
		{
			double v = 0.0;
			if (sscanf(ln.c_str(), "dt %lf", &v) == 1 && v > 0.0)
				s.dts.push_back(v); // B11 FR_PLAY：录制节奏（解析序=帧序）
			continue; // 离线无 kernel，dt 只记录
		}
		if (ln.compare(0, 6, "s_crc ") == 0)
			continue; // 同上：s_crc 是内核堆快照，离线无 kernel 只跳过（B6）
		if (ln.compare(0, 6, "d_crc ") == 0)
			continue; // 同上：d_crc 是录制侧 out 行 CRC（B7a），离线执行域只重算 D_CRC，不消费
		if (ln.compare(0, 6, "asset ") == 0)
		{
			if (!offline_register_asset_text(ln, no))
			{
				std::fprintf(stderr, "fr-offline: 第 %d 行：资产行非法\n", no);
				return 3;
			}
			continue;
		}
		if (ln.compare(0, 6, "clear ") == 0 || ln.compare(0, 5, "draw ") == 0
		    || ln.compare(0, 6, "print ") == 0 || ln.compare(0, 5, "rect ") == 0
		    || ln.compare(0, 5, "line ") == 0 || ln.compare(0, 7, "circle ") == 0
		    || ln.compare(0, 5, "poly ") == 0 || ln.compare(0, 4, "arc ") == 0
		    || ln.compare(0, 7, "points ") == 0
		    || ln.compare(0, 3, "rt ") == 0 || ln.compare(0, 8, "scissor ") == 0)
		{
			s.frames.back().push_back(ln);
			continue;
		}
		std::fprintf(stderr, "fr-offline: 第 %d 行：离线回放不支持的非 out 行（B8 op 域）\n", no);
		return 3;
	}
	return 0;
}

// 帧体（二进制）→ 会话：fr_dec 游标逐行解码；资产直接登记，out 行重渲染
// 为 canonical 文本走 capture_exec_row（与 tee₂ 文本路径同一执行入口）。
// base = 帧体在文件中的起始偏移（错误信息换算绝对偏移用）。
static int offline_parse_frame_body(const love::uint8 *body, size_t n, size_t base,
                                    std::vector<std::string> &out_rows)
{
	fr_dec d;
	fr_dec_init(&d, body, n);
	for (;;)
	{
		int r = fr_dec_next(&d);
		if (r == 0)
			break;
		if (r < 0)
		{
			std::fprintf(stderr, "fr-offline: off %d：行解码失败（码 %d）\n",
			             (int) (base + d.err_off), d.err);
			return 3;
		}

		char text[512];
		switch (d.op)
		{
		case FR_OP_DT:
			(void) fr_dec_f32(&d); // 无 kernel，dt 只消费
			break;
		case FR_OP_S_CRC:
			// 无 kernel，堆快照 CRC 只消费（B6：tee 会话 end 段行放行）。
			(void) fr_dec_u32(&d);
			break;
		case FR_OP_D_CRC:
			// 同 s_crc：录制侧 out 行 CRC 只消费（B7a：tee 会话 end 段行放行）。
			(void) fr_dec_u32(&d);
			break;
		case FR_OP_ASSET:
			if (d.asset_kind == FR_ASSET_KIND_IMAGE)
			{
				love::uint32 id = fr_dec_u32(&d), w = fr_dec_u32(&d), h = fr_dec_u32(&d);
				(void) fr_dec_u32(&d); // data 长度（pending_len 供 fr_dec_data）
				const love::uint8 *p = nullptr;
				love::uint32 len = 0;
				if (fr_dec_data(&d, &p, &len) != 0)
					break;
				fr_register_stream_asset(FR_ASSET_IMAGE, id, (int) w, (int) h, 0.0f,
				                         nullptr, hex_encode(p, len));
			}
			else if (d.asset_kind == FR_ASSET_KIND_CANVAS)
			{
				love::uint32 id = fr_dec_u32(&d), w = fr_dec_u32(&d), h = fr_dec_u32(&d);
				fr_register_stream_asset(FR_ASSET_CANVAS, id, (int) w, (int) h, 0.0f,
				                         nullptr, "");
			}
			else if (d.asset_kind == FR_ASSET_KIND_QUAD)
			{
				love::uint32 id = fr_dec_u32(&d);
				float q[6];
				for (int k = 0; k < 6; k++)
					q[k] = fr_dec_f32(&d);
				fr_register_stream_asset(FR_ASSET_QUAD, id, 0, 0, 0.0f, q, "");
			}
			else if (d.asset_kind == FR_ASSET_KIND_FONT)
			{
				love::uint32 id = fr_dec_u32(&d);
				float size = fr_dec_f32(&d);
				(void) fr_dec_u32(&d); // data 长度
				const love::uint8 *p = nullptr;
				love::uint32 len = 0;
				if (fr_dec_data(&d, &p, &len) != 0)
					break;
				fr_register_stream_asset(FR_ASSET_FONT, id, 0, 0, size,
				                         nullptr, hex_encode(p, len));
			}
			else
			{
				std::fprintf(stderr, "fr-offline: off %d：asset kind 非法 %d\n",
				             (int) (base + d.off), (int) d.asset_kind);
				return 3;
			}
			break;
		case FR_OP_CLEAR:
		{
			fr_rgba8 c;
			if (fr_dec_rgba8(&d, &c) != 0)
				break;
			out_rows.push_back(render_clear_row(c.r, c.g, c.b, c.a));
			break;
		}
		case FR_OP_DRAW:
		{
			love::uint32 tex = fr_dec_u32(&d), quad = fr_dec_u32(&d);
			float x = fr_dec_f32(&d), y = fr_dec_f32(&d), rot = fr_dec_f32(&d);
			float sx = fr_dec_f32(&d), sy = fr_dec_f32(&d);
			float ox = fr_dec_f32(&d), oy = fr_dec_f32(&d);
			fr_rgba8 c;
			if (fr_dec_rgba8(&d, &c) != 0)
				break;
			out_rows.push_back(render_draw_row(tex, quad, x, y, rot, sx, sy, ox, oy,
			                                   c.r, c.g, c.b, c.a));
			break;
		}
		case FR_OP_PRINT:
		{
			love::uint32 font = fr_dec_u32(&d);
			if (fr_dec_str(&d, text, sizeof(text)) != 0)
				break;
			float x = fr_dec_f32(&d), y = fr_dec_f32(&d), rot = fr_dec_f32(&d);
			float sx = fr_dec_f32(&d), sy = fr_dec_f32(&d);
			fr_rgba8 c;
			if (fr_dec_rgba8(&d, &c) != 0)
				break;
			out_rows.push_back(render_print_row(font, std::string(text), x, y, rot,
			                                    sx, sy, c.r, c.g, c.b, c.a));
			break;
		}
		case FR_OP_RECT:
		{
			love::uint8 mode = fr_dec_u8(&d);
			float x = fr_dec_f32(&d), y = fr_dec_f32(&d), w = fr_dec_f32(&d);
			float h = fr_dec_f32(&d), rx = fr_dec_f32(&d), ry = fr_dec_f32(&d);
			float width = fr_dec_f32(&d);
			fr_rgba8 c;
			if (fr_dec_rgba8(&d, &c) != 0)
				break;
			out_rows.push_back(render_rect_row(mode, x, y, w, h, rx, ry, width,
			                                   c.r, c.g, c.b, c.a));
			break;
		}
		case FR_OP_LINE:
		{
			// line pts f32[] width f32 color rgba8
			love::uint32 n = 0;
			float pbuf[512]; const float *pf = pbuf;
			float width = 0;
			fr_rgba8 c;
			if (fr_dec_pts(&d, pbuf, 512, &n) != 0) break;
			width = fr_dec_f32(&d);
			if (fr_dec_rgba8(&d, &c) != 0) break;
			std::string row = "line " + std::to_string(n);
			for (love::uint32 i = 0; i < n * 2; i++)
			{
				char b[32]; snprintf(b, sizeof(b), " %.6f", pf[i]);
				row += b;
			}
			char tail[48];
			snprintf(tail, sizeof(tail), " %.6f %d %d %d %d", width, (int) c.r, (int) c.g, (int) c.b, (int) c.a);
			row += tail;
			out_rows.push_back(row);
			break;
		}
		case FR_OP_CIRCLE:
		{
			love::uint8 mode = fr_dec_u8(&d);
			float x = fr_dec_f32(&d), y = fr_dec_f32(&d), r = fr_dec_f32(&d);
			love::uint32 seg = fr_dec_u32(&d);
			fr_rgba8 c;
			if (fr_dec_rgba8(&d, &c) != 0) break;
			char buf[160];
			snprintf(buf, sizeof(buf), "circle %u %.6f %.6f %.6f %u %d %d %d %d",
			         (unsigned) mode, x, y, r, (unsigned) seg, (int) c.r, (int) c.g, (int) c.b, (int) c.a);
			out_rows.push_back(buf);
			break;
		}
		case FR_OP_POLY:
		{
			love::uint8 mode = fr_dec_u8(&d);
			love::uint32 n = 0;
			float pbuf[512]; const float *pf = pbuf;
			fr_rgba8 c;
			if (fr_dec_pts(&d, pbuf, 512, &n) != 0) break;
			if (fr_dec_rgba8(&d, &c) != 0) break;
			std::string row = "poly " + std::to_string((unsigned) mode) + " " + std::to_string(n);
			for (love::uint32 i = 0; i < n * 2; i++)
			{
				char b[32]; snprintf(b, sizeof(b), " %.6f", pf[i]);
				row += b;
			}
			char tail[32];
			snprintf(tail, sizeof(tail), " %d %d %d %d", (int) c.r, (int) c.g, (int) c.b, (int) c.a);
			row += tail;
			out_rows.push_back(row);
			break;
		}
		case FR_OP_ARC:
		{
			love::uint8 mode = fr_dec_u8(&d), arctype = fr_dec_u8(&d);
			float x = fr_dec_f32(&d), y = fr_dec_f32(&d), r = fr_dec_f32(&d);
			float a1 = fr_dec_f32(&d), a2 = fr_dec_f32(&d);
			love::uint32 seg = fr_dec_u32(&d);
			fr_rgba8 c;
			if (fr_dec_rgba8(&d, &c) != 0) break;
			char buf[200];
			snprintf(buf, sizeof(buf), "arc %u %u %.6f %.6f %.6f %.6f %.6f %u %d %d %d %d",
			         (unsigned) mode, (unsigned) arctype, x, y, r, a1, a2,
			         (unsigned) seg, (int) c.r, (int) c.g, (int) c.b, (int) c.a);
			out_rows.push_back(buf);
			break;
		}
		case FR_OP_POINTS:
		{
			love::uint32 n = 0;
			float pbuf[512]; const float *pf = pbuf;
			float size = 0;
			fr_rgba8 c;
			if (fr_dec_pts(&d, pbuf, 512, &n) != 0) break;
			size = fr_dec_f32(&d);
			if (fr_dec_rgba8(&d, &c) != 0) break;
			std::string row = "points " + std::to_string(n);
			for (love::uint32 i = 0; i < n * 2; i++)
			{
				char b[32]; snprintf(b, sizeof(b), " %.6f", pf[i]);
				row += b;
			}
			char tail[48];
			snprintf(tail, sizeof(tail), " %.6f %d %d %d %d", size, (int) c.r, (int) c.g, (int) c.b, (int) c.a);
			row += tail;
			out_rows.push_back(row);
			break;
		}
		case FR_OP_RT: // B8 T3：rt target u32（0=会话基线，见 capture_exec_row）
		{
			love::uint32 target = fr_dec_u32(&d);
			out_rows.push_back(render_rt_row(target));
			break;
		}
		case FR_OP_SCISSOR: // B8 T3：scissor x y w h（-1 哨兵=off）
		{
			float x = fr_dec_f32(&d), y = fr_dec_f32(&d);
			float w = fr_dec_f32(&d), h = fr_dec_f32(&d);
			out_rows.push_back(render_scissor_row(x, y, w, h));
			break;
		}
		default:
			std::fprintf(stderr, "fr-offline: off %d：离线回放不支持的非 out 行 "
			             "opcode 0x%02X（B8 op 域）\n", (int) (base + d.off - 1), d.op);
			return 3;
		}

		if (d.err)
		{
			std::fprintf(stderr, "fr-offline: off %d：行字段解码失败（码 %d）\n",
			             (int) (base + d.err_off), d.err);
			return 3;
		}
	}
	return 0;
}

// 二进制（.frb，小端）→ 会话。容器口径对齐 tools/fr_session.parse_binary：
// 头 50B（FRB1 + ver + w/h u16 + sha 32B + loglvl + seed u64）+ entries
//（tag 0xFF cmnt / 0x01 frame，各带 u32 长度前缀）；错误含偏移（0 起计）。
// 行级解码走 fr_dec（fr-gen 生成物）——本层不手写行编解码。
static int offline_parse_binary(const std::string &buf, OfflineSession &s)
{
	static const size_t HEADER_LEN = 50;
	if (buf.size() < HEADER_LEN)
	{
		std::fprintf(stderr, "fr-offline: off 0：头部截断（需 %d 字节，得 %d）\n",
		             (int) HEADER_LEN, (int) buf.size());
		return 3;
	}
	if ((love::uint8) buf[4] != 1)
	{
		std::fprintf(stderr, "fr-offline: off 4：版本不支持 %d\n", (int) (love::uint8) buf[4]);
		return 3;
	}
	love::uint32 w = (love::uint32) (love::uint8) buf[5]
	                | ((love::uint32) (love::uint8) buf[6] << 8);
	love::uint32 h = (love::uint32) (love::uint8) buf[7]
	                | ((love::uint32) (love::uint8) buf[8] << 8);
	s.width = (int) w;
	s.height = (int) h;

	const love::uint8 *p = (const love::uint8 *) buf.data();
	size_t len = buf.size(), off = HEADER_LEN;
	while (off < len)
	{
		love::uint8 tag = p[off];
		off++;
		if (off + 4 > len)
		{
			std::fprintf(stderr, "fr-offline: off %d：entry 长度截断\n", (int) off);
			return 3;
		}
		love::uint32 n = (love::uint32) p[off] | ((love::uint32) p[off + 1] << 8)
		               | ((love::uint32) p[off + 2] << 16) | ((love::uint32) p[off + 3] << 24);
		off += 4;
		if (n > (love::uint32) (len - off))
		{
			std::fprintf(stderr, "fr-offline: off %d：entry 截断（需 %d 字节，余 %d）\n",
			             (int) off, (int) n, (int) (len - off));
			return 3;
		}
		if (tag == 0xFF)
		{
			off += n; // cmnt：离线无消费，跳过
			continue;
		}
		if (tag != 0x01)
		{
			std::fprintf(stderr, "fr-offline: off %d：entry tag 非法 0x%02X\n",
			             (int) (off - 5), (int) tag);
			return 3;
		}
		s.frames.push_back(std::vector<std::string>());
		int rc = offline_parse_frame_body(p + off, n, off, s.frames.back());
		off += n;
		if (rc != 0)
			return rc;
	}
	return 0;
}

int fr_offline_replay(lua_State *L, const char *path, love::graphics::Graphics *gfx)
{
	static const bool g_fr_play = []()
	{
		const char *v = getenv("FR_PLAY");
		return v != nullptr && std::string(v) == "1";
	}();
	if (gfx == nullptr)
	{
		std::fprintf(stderr, "fr-offline: graphics 模块缺席\n");
		return 2;
	}

	FILE *f = std::fopen(path, "rb");
	if (f == nullptr)
	{
		std::fprintf(stderr, "fr-offline: 打不开流文件 %s\n", path);
		return 2;
	}
	std::string buf;
	char chunk[65536];
	size_t n;
	while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
		buf.append(chunk, n);
	bool io_err = std::ferror(f) != 0;
	std::fclose(f);
	if (io_err)
	{
		std::fprintf(stderr, "fr-offline: 读流文件失败 %s\n", path);
		return 2;
	}

	OfflineSession s;
	int rc = (buf.size() >= 4 && buf.compare(0, 4, "FRB1") == 0)
	             ? offline_parse_binary(buf, s)
	             : offline_parse_text(buf, s);
	if (rc != 0)
		return rc;
	if (s.frames.empty())
	{
		std::fprintf(stderr, "fr-offline: 会话无帧\n");
		return 3;
	}

	// 画布（_frNewCanvasReal 同款：会话头几何、dpiScale=1 定像素域）。
	// 帧间不重建——行执行是累积语义（live 的 _frReplay 重放全会话行缓冲
	// 与此同构；每帧块首 clear 保证帧内容独立性）。
	love::graphics::Canvas::Settings settings;
	settings.width = s.width;
	settings.height = s.height;
	settings.dpiScale = 1.0f;
	love::graphics::Canvas *canvas = nullptr;
	try
	{
		canvas = gfx->newCanvas(settings);
	}
	catch (love::Exception &e)
	{
		std::fprintf(stderr, "fr-offline: newCanvas 失败：%s\n", e.what());
		return 2;
	}

	love::image::Image *imagemod =
		love::Module::getInstance<love::image::Image>(love::Module::M_IMAGE);

	for (size_t fi = 0; fi < s.frames.size(); fi++)
	{
		try
		{
			love::graphics::Graphics::RenderTargets rts;
			rts.colors.emplace_back(canvas, 0);
			gfx->setCanvas(rts);
			g_replay_baseline = rts;      // B8 T3：离线画布即会话基线（rt 0 归位）
			g_replay_baseline_canvas = true;
			for (const std::string &row : s.frames[fi])
				capture_exec_row(gfx, row);
			g_replay_baseline_canvas = false;
			gfx->setCanvas(); // 解绑后再回读（active Canvas 不许 newImageData）

			love::Rect rect = {0, 0, canvas->getPixelWidth(), canvas->getPixelHeight()};
			love::image::ImageData *img = canvas->newImageData(imagemod, 0, 0, rect);

			// B11 FR_PLAY=1：视觉播放——离线画布贴屏幕（缩放适配窗口）+
			// 按录制 dt 节奏 sleep + present。默认关（CI 判据路径零 sleep
			// 零上屏——"持续黑窗"是拓扑 B 无头验证的既定形态，非 bug）。
			if (g_fr_play)
			{
				gfx->setCanvas();
				gfx->origin();
				float sx = (float) gfx->getWidth() / (float) canvas->getPixelWidth();
				float sy = (float) gfx->getHeight() / (float) canvas->getPixelHeight();
				gfx->setColor(love::Colorf(1.0f, 1.0f, 1.0f, 1.0f));
				love::Matrix4 m(0.0f, 0.0f, 0.0f, sx, sy, 0.0f, 0.0f, 0.0f, 0.0f);
				gfx->draw((love::graphics::Drawable *) canvas, m);
				love::timer::Timer *timer =
					love::Module::getInstance<love::timer::Timer>(love::Module::M_TIMER);
				double dt = fi < s.dts.size() ? s.dts[fi] : (1.0 / 60.0);
				if (timer != nullptr)
					timer->sleep(dt);
				gfx->present(L);
			}

			// D_CRC n = 前 n 个帧块执行后的画布 CRC（live 验证帧计数同口径：
			// live 第 3 帧回读时行缓冲恰为前 3 帧块）。
			std::printf("D_CRC %d %u\n", (int) fi + 1,
			            fr_crc32_bytes(img->getData(), img->getSize()));

			if (fi + 1 == s.frames.size())
			{
				// 三采样像素（live 验证帧同点同换算：getPixel + floor(c*255+0.5)）
				static const int pts[3][2] = {{400, 100}, {20, 30}, {320, 468}};
				for (int k = 0; k < 3; k++)
				{
					love::Colorf c = img->getPixel(pts[k][0], pts[k][1]);
					int r = (int) std::floor(c.r * 255.0f + 0.5f);
					int g = (int) std::floor(c.g * 255.0f + 0.5f);
					int b = (int) std::floor(c.b * 255.0f + 0.5f);
					std::printf("PIXEL %d %d %d %d %d\n", pts[k][0], pts[k][1], r, g, b);
				}
			}
			img->release();
		}
		catch (love::Exception &e)
		{
			gfx->setCanvas();
			canvas->release();
			std::fprintf(stderr, "fr-offline: 第 %d 帧回放失败：%s\n",
			             (int) fi + 1, e.what());
			return 2;
		}
	}
	canvas->release();
	std::fflush(stdout);
	// FR_PLAY 播完短暂停留（末帧人眼可辨；短流播放瞬完即退的问题）。
	if (g_fr_play)
	{
		love::timer::Timer *timer =
			love::Module::getInstance<love::timer::Timer>(love::Module::M_TIMER);
		if (timer != nullptr)
			timer->sleep(1.0);
	}
	return 0;
}

int w_frImageDataCRC(lua_State *L)
{
	love::image::ImageData *img = love::luax_checktype<love::image::ImageData>(L, 1);
	lua_pushinteger(L, (lua_Integer) fr_crc32_bytes(img->getData(), img->getSize()));
	return 1;
}

// B7a：字符串字节 CRC32——demo 侧 d_crc 行（当帧 out 行文本）的算法出口。
// 行集口径（clear/draw/print/rect，take_delta 同批）与 join 语义归调用方
// （demo 拼串）；算法单一真源在 C（fr_crc32_bytes 与 D_CRC/s_crc 同一
// CRC32 机，demo 不另置实现）。
int w_frCRC32(lua_State *L)
{
	size_t len = 0;
	const char *s = luaL_checklstring(L, 1, &len);
	lua_pushinteger(L, (lua_Integer) fr_crc32_bytes(s, len));
	return 1;
}

// 验证帧三点回读 + 汇总 + D_CRC（B6 下沉 C 侧，行格式与原 Lua 环逐字节
// 同构：`PIXEL x y r g b`×3 + `PIXELS 3 n` + `D_CRC <frame> <crc>`）。
// 三采样点/期望色 = 既有像素判据锚（tests/test_capture._POINTS 同源），
// rig 常量落 C；换算 = getPixel 的 0-1 浮点 × floor(c*255+0.5)。
int w_frCanvasProbe(lua_State *L)
{
	love::graphics::Canvas *c = love::luax_checktype<love::graphics::Canvas>(L, 1);
	int frameno = (int) luaL_optinteger(L, 2, 0);

	love::image::Image *imagemod =
		love::Module::getInstance<love::image::Image>(love::Module::M_IMAGE);
	love::Rect rect = {0, 0, c->getPixelWidth(), c->getPixelHeight()};
	love::image::ImageData *img = nullptr;
	love::luax_catchexcept(L, [&]() { img = c->newImageData(imagemod, 0, 0, rect); });

	static const int pts[3][2] = {{400, 100}, {20, 30}, {320, 468}};
	static const int want[3][3] = {{0, 0, 0}, {255, 0, 0}, {255, 0, 0}};
	int npass = 0;
	for (int k = 0; k < 3; k++)
	{
		love::Colorf col = img->getPixel(pts[k][0], pts[k][1]);
		int r = (int) std::floor(col.r * 255.0f + 0.5f);
		int g = (int) std::floor(col.g * 255.0f + 0.5f);
		int b = (int) std::floor(col.b * 255.0f + 0.5f);
		std::printf("PIXEL %d %d %d %d %d\n", pts[k][0], pts[k][1], r, g, b);
		if (r == want[k][0] && g == want[k][1] && b == want[k][2])
			npass++;
	}
	std::printf("PIXELS %d %d\n", 3, npass);
	std::printf("D_CRC %d %u\n", frameno,
	            fr_crc32_bytes(img->getData(), img->getSize()));
	std::fflush(stdout);
	img->release();
	return 0;
}

int w_frOfflineReplay(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	love::graphics::Graphics *gfx =
		love::Module::getInstance<love::graphics::Graphics>(love::Module::M_GRAPHICS);
	lua_pushinteger(L, fr_offline_replay(L, path, gfx));
	return 1;
}

} // fr
