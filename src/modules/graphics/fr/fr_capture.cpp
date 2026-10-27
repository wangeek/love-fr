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
#include "image/ImageData.h"

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

	char buf[64];
	snprintf(buf, sizeof(buf), "clear %d %d %d %d",
	         rgba8(c->value.r), rgba8(c->value.g),
	         rgba8(c->value.b), rgba8(c->value.a));
	rows().push_back(buf);
}

void capture_draw(love::uint32 texid, love::uint32 quadid, float x, float y, float rot,
                  float sx, float sy, float ox, float oy, const love::Colorf &color)
{
	char buf[256];
	snprintf(buf, sizeof(buf),
	         "draw %u %u %.6f %.6f %.6f %.6f %.6f %.6f %.6f %d %d %d %d",
	         texid, quadid, x, y, rot, sx, sy, ox, oy,
	         rgba8(color.r), rgba8(color.g), rgba8(color.b), rgba8(color.a));
	rows().push_back(buf);
}

void capture_print(love::uint32 fontid, const std::string &text, float x, float y,
                   float rot, float sx, float sy, const love::Colorf &color)
{
	char buf[512];
	snprintf(buf, sizeof(buf),
	         "print %u \"%s\" %.6f %.6f %.6f %.6f %.6f %d %d %d %d",
	         fontid, esc(text).c_str(), x, y, rot, sx, sy,
	         rgba8(color.r), rgba8(color.g), rgba8(color.b), rgba8(color.a));
	rows().push_back(buf);
}

void capture_rect(love::uint8 mode, float x, float y, float w, float h, float rx,
                  float ry, float width, const love::Colorf &color)
{
	char buf[256];
	snprintf(buf, sizeof(buf),
	         "rect %u %.6f %.6f %.6f %.6f %.6f %.6f %.6f %d %d %d %d",
	         (unsigned) mode, x, y, w, h, rx, ry, width,
	         rgba8(color.r), rgba8(color.g), rgba8(color.b), rgba8(color.a));
	rows().push_back(buf);
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
// 回放（present 钩 / _frReplay 入口）
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

void capture_replay(love::graphics::Graphics *gfx)
{
	if (gfx == nullptr)
		return;

	for (const std::string &row : rows())
	{
		// .fr 行 = 空格分隔 token，首 token 为 op 名（golden/l0_session.fr 同构）。
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
				if (rx > 0.0f || ry > 0.0f)
					gfx->rectangle(m, x, y, w, h, rx, ry); // 圆角 points 缺省（行内无此字段）
				else
					gfx->rectangle(m, x, y, w, h);
			}
		}
	}
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

} // fr
