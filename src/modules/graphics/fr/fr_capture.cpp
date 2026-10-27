/**
 * FR 捕获基础设施实现（B4-2 fork 魔改）——边界与行格式见 fr_capture.h。
 **/

#include "modules/graphics/fr/fr_capture.h"

// LOVE
#include "common/Module.h"
#include "common/runtime.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace fr
{

static std::vector<std::string> &rows()
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

static int rgba8(float v)
{
	if (v < 0.0f)
		v = 0.0f;
	if (v > 1.0f)
		v = 1.0f;
	return (int) lroundf(v * 255.0f);
}

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

		// 其它 op（draw/print/rect/...）：Task 3 扩展。
	}
}

std::vector<std::string> capture_take_rows()
{
	return rows();
}

int w_frTakeSession(lua_State *L)
{
	const std::vector<std::string> &rs = rows();
	lua_createtable(L, (int) rs.size(), 0);
	for (size_t i = 0; i < rs.size(); i++)
	{
		lua_pushlstring(L, rs[i].data(), rs[i].size());
		lua_rawseti(L, -2, (int) i + 1);
	}
	return 1;
}

int w_frReplay(lua_State *)
{
	love::graphics::Graphics *gfx =
		love::Module::getInstance<love::graphics::Graphics>(love::Module::M_GRAPHICS);
	capture_replay(gfx);
	return 0;
}

} // fr
