/**
 * FR 捕获基础设施实现（B4-2 fork 魔改）——边界与行格式见 fr_capture.h。
 **/

#include "modules/graphics/fr/fr_capture.h"
#include "modules/graphics/fr/fr_materialize.h"

// LOVE
#include "common/Exception.h"
#include "common/Module.h"
#include "common/runtime.h"
#include "filesystem/Filesystem.h"
#include "graphics/Canvas.h"
#include "image/Image.h"
#include "image/ImageData.h"
#include "modules/graphics/fr/fr_codec.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

bool capture_enabled()
{
	static const bool enabled = []()
	{
		const char *v = getenv("FR_CAPTURE");
		return v != nullptr && std::string(v) == "1";
	}();
	return enabled;
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

static int proxy_index(lua_State *L)
{
	FrProxy *p = (FrProxy *) lua_touserdata(L, 1);
	const char *k = luaL_checkstring(L, 2);
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
	return luaL_error(L, "fr.asset proxy: 未知字段 \"%s\"（可用 id/kind）", k);
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
	assets().push_back(a);
	asset_rows().push_back(render_asset_row(a));
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
	register_asset(a);
	push_proxy(L, a);
	return 1;
}

int w_frNewFont(lua_State *L)
{
	// demo 边界：仅 newFont(size) 形态。默认字体 = Font.cpp 内嵌 Vera.ttf
	//（DefaultFontData）——登记其真字节 + size（B4 阶段三；物化侧
	// newTrueTypeRasterizer 同链重建）。文件/Rasterizer 形态归后续批次。
	if (lua_type(L, 1) != LUA_TNUMBER)
		return luaL_error(L, "fr.newFont: 语义替代仅支持 newFont(size) 形态（B4 demo 边界）");

	FrAsset a;
	a.kind = FR_ASSET_FONT;
	a.size = (float) luaL_checknumber(L, 1);
	a.data_hex = hex_encode(love::font::Vera_ttf, love::font::Vera_ttf_len);
	a.id = next_asset_id();
	register_asset(a);
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
	register_asset(a);
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
	register_asset(a);
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

// 单行执行：.fr 行 = 空格分隔 token，首 token 为 op 名（golden/l0_session.fr
// 同构）。tee₂（capture_replay）与拓扑 B 离线回放共用本入口——同一物化器、
// 同一真渲染路径（B4-2 验收线第二分句的同构关键）。
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
			love::graphics::Image *img =
				(quad == 0) ? fr_materialize_image(gfx, tex) : nullptr;
			if (img != nullptr)
			{
				// 裸参数矩阵重建（luax_checkstandardtransform 的
				// Matrix4(x,y,a,sx,sy,ox,oy,kx,ky) 同构；x/y 已是捕获时
				// 结算的世界系，回放变换栈恒 origin，无二次变换）。
				love::Matrix4 m(x, y, rot, sx, sy, ox, oy, 0.0f, 0.0f);
				gfx->setColor(love::Colorf(r / 255.0f, g / 255.0f,
				                           b / 255.0f, a / 255.0f));
				gfx->draw((love::graphics::Drawable *) img, m);
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
}

void capture_replay(love::graphics::Graphics *gfx)
{
	if (gfx == nullptr)
		return;

	for (const std::string &row : rows())
		capture_exec_row(gfx, row);
}

// ------------------------------------------------------------------
// Lua 出口
// ------------------------------------------------------------------

std::vector<std::string> capture_take_rows()
{
	return rows();
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
			continue; // 离线无 kernel，dt 只跳过
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
		    || ln.compare(0, 6, "print ") == 0 || ln.compare(0, 5, "rect ") == 0)
		{
			s.frames.back().push_back(ln);
			continue;
		}
		std::fprintf(stderr, "fr-offline: 第 %d 行：离线回放不支持的非 out 行（B4 op 域）\n", no);
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
		default:
			std::fprintf(stderr, "fr-offline: off %d：离线回放不支持的非 out 行 "
			             "opcode 0x%02X（B4 op 域）\n", (int) (base + d.off - 1), d.op);
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

int fr_offline_replay(const char *path, love::graphics::Graphics *gfx)
{
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
			for (const std::string &row : s.frames[fi])
				capture_exec_row(gfx, row);
			gfx->setCanvas(); // 解绑后再回读（active Canvas 不许 newImageData）

			love::Rect rect = {0, 0, canvas->getPixelWidth(), canvas->getPixelHeight()};
			love::image::ImageData *img = canvas->newImageData(imagemod, 0, 0, rect);

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
	lua_pushinteger(L, fr_offline_replay(path, gfx));
	return 1;
}

} // fr
