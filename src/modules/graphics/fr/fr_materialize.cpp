/**
 * FR 真纹理物化器实现（B4 阶段三）——物化链与失败面见 fr_materialize.h。
 **/

#include "modules/graphics/fr/fr_materialize.h"

#include "modules/graphics/fr/fr_capture.h"

// LOVE
#include "common/Exception.h"
#include "common/Module.h"
#include "common/Object.h"
#include "filesystem/FileData.h"
#include "font/Font.h"
#include "graphics/Font.h"
#include "graphics/Graphics.h"
#include "graphics/Image.h"
#include "image/Image.h"
#include "image/ImageData.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fr
{

// ------------------------------------------------------------------
// hex → bytes
// ------------------------------------------------------------------

static int hexnib(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

// 非法（奇长/坏字符）→ 空 vector（调用方按物化失败处理）。
static std::vector<love::uint8> unhex(const std::string &hex)
{
	std::vector<love::uint8> out;
	if (hex.size() % 2 != 0)
		return out;
	out.reserve(hex.size() / 2);
	for (size_t i = 0; i + 1 < hex.size(); i += 2)
	{
		int hi = hexnib(hex[i]);
		int lo = hexnib(hex[i + 1]);
		if (hi < 0 || lo < 0)
		{
			out.clear();
			return out;
		}
		out.push_back((love::uint8) ((hi << 4) | lo));
	}
	return out;
}

// ------------------------------------------------------------------
// 物化缓存 + 一次性告警
// ------------------------------------------------------------------

static std::map<love::uint32, love::StrongRef<love::graphics::Image>> &image_cache()
{
	static std::map<love::uint32, love::StrongRef<love::graphics::Image>> m;
	return m;
}

static std::map<love::uint32, love::StrongRef<love::graphics::Font>> &font_cache()
{
	static std::map<love::uint32, love::StrongRef<love::graphics::Font>> m;
	return m;
}

static std::map<love::uint32, love::StrongRef<love::graphics::Canvas>> &canvas_cache()
{
	static std::map<love::uint32, love::StrongRef<love::graphics::Canvas>> m;
	return m;
}

// B9：退出冲刷——love.cpp 在 lua_close 前调（GL 上下文/Graphics 模块仍
// 活着）。静态缓存 StrongRef 的析构若落到 atexit 阶段（LIFO 晚于模块
// teardown），会撞已死上下文——newgame 实测退出 SIGSEGV（atexit vector
// release 链悬垂，探针 RVA 0x1203f2）。幂等。
void fr_materialize_flush()
{
	image_cache().clear();
	font_cache().clear();
	canvas_cache().clear();
}

// stderr 告警按 id 一次性（行缓冲全会话累积、present 每帧重放——不收口会刷屏）。
static void warn_once(love::uint32 id, const char *why)
{
	static std::set<love::uint32> warned;
	if (warned.insert(id).second)
		std::fprintf(stderr, "fr: materialize id=%u 失败（%s），回退占位\n", id, why);
}

// ------------------------------------------------------------------
// image 物化：hex → ImageData → Slices → newImage（w_newImage 同链）
// ------------------------------------------------------------------

love::graphics::Image *fr_materialize_image(love::graphics::Graphics *gfx, love::uint32 id)
{
	if (gfx == nullptr)
		return nullptr;

	auto it = image_cache().find(id);
	if (it != image_cache().end())
		return it->second.get();

	FrAssetInfo info;
	if (!fr_lookup_asset(id, info))
	{
		warn_once(id, "id 未登记");
		return nullptr;
	}
	if (info.kind != FR_ASSET_IMAGE)
	{
		warn_once(id, "非 image 资产");
		return nullptr;
	}
	if (info.w <= 0 || info.h <= 0 || info.data_hex.empty())
	{
		warn_once(id, "无真字节登记");
		return nullptr;
	}

	std::vector<love::uint8> bytes = unhex(info.data_hex);

	love::image::Image *im =
		love::Module::getInstance<love::image::Image>(love::Module::M_IMAGE);
	if (im == nullptr)
	{
		warn_once(id, "image 模块缺席");
		return nullptr;
	}

	love::image::ImageData *d = nullptr;
	// B12 瘦身修订：登记字节两口径——魔数命中（PNG/BMP/JPEG）= 编码文件
	// 字节，FileData 承载走 newImageData(Data) 解码（登记侧不再膨胀成
	// RGBA8 像素域，路径资产 5-10×）；否则 = RGBA8 原始像素（ImageData
	// 形态登记），直构并做 w*h*4 长度校验。
	static const love::uint8 kPng[4] = {0x89, 0x50, 0x4E, 0x47};
	static const love::uint8 kBmp[2] = {0x42, 0x4D};
	static const love::uint8 kJpg[2] = {0xFF, 0xD8};
	bool encoded = bytes.size() >= 4
	               && (std::memcmp(bytes.data(), kPng, 4) == 0
	                   || std::memcmp(bytes.data(), kBmp, 2) == 0
	                   || std::memcmp(bytes.data(), kJpg, 2) == 0);
	if (encoded)
	{
		try
		{
			love::filesystem::FileData *fd =
				new love::filesystem::FileData(bytes.size(), "fr_materialize_image");
			std::memcpy(fd->getData(), bytes.data(), bytes.size());
			d = im->newImageData(fd);
			fd->release();
		}
		catch (love::Exception &)
		{
			warn_once(id, "编码字节解码抛异常");
			return nullptr;
		}
	}
	else
	{
		if (bytes.size() != (size_t) info.w * info.h * 4)
		{
			warn_once(id, "hex 长度 ≠ w*h*4（RGBA8 域）");
			return nullptr;
		}
		try
		{
			// own=false（缺省）→ 拷贝进 ImageData，bytes 生命周期本函数内即可。
			d = im->newImageData(info.w, info.h, love::PIXELFORMAT_RGBA8, bytes.data());
		}
		catch (love::Exception &)
		{
			warn_once(id, "newImageData 抛异常");
			return nullptr;
		}
	}
	if (d == nullptr)
	{
		warn_once(id, "解码返回空");
		return nullptr;
	}

	love::graphics::Image::Slices slices(love::graphics::TEXTURE_2D);
	slices.set(0, 0, d);
	d->release();

	love::graphics::Image *img = nullptr;
	try
	{
		// Settings 缺省：无 mipmap/linear，dpiScale=1（像素域=逻辑域，
		// _frNewCanvasReal 同口径——不随窗口 DPI 漂移）。
		love::graphics::Image::Settings settings;
		img = gfx->newImage(slices, settings);
	}
	catch (love::Exception &)
	{
		warn_once(id, "newImage 抛异常");
		return nullptr;
	}

	// newImage 返回即持一引用（调用方所有）→ NORETAIN 移交缓存。
	image_cache()[id] = love::StrongRef<love::graphics::Image>(img, love::Acquire::NORETAIN);
	return img;
}

// ------------------------------------------------------------------
// font 物化：hex → FileData → newTrueTypeRasterizer → newFont
//（w_newFont/w_newTrueTypeRasterizer 同链）
// ------------------------------------------------------------------

love::graphics::Font *fr_materialize_font(love::graphics::Graphics *gfx, love::uint32 id)
{
	if (gfx == nullptr)
		return nullptr;

	auto it = font_cache().find(id);
	if (it != font_cache().end())
		return it->second.get();

	FrAssetInfo info;
	if (!fr_lookup_asset(id, info))
	{
		warn_once(id, "id 未登记");
		return nullptr;
	}
	if (info.kind != FR_ASSET_FONT)
	{
		warn_once(id, "非 font 资产");
		return nullptr;
	}
	// B12+ 字节引用（v1.2.7 池化）：data_hex 空且 font_src>0 → 取源资产字节
	//（源必为自有字节者——登记侧保证不链式），size 用本行（光栅化参数）。
	if (info.data_hex.empty() && info.font_src != 0)
	{
		FrAssetInfo src;
		if (!fr_lookup_asset(info.font_src, src) || src.data_hex.empty())
		{
			warn_once(id, "font 引用源无字节");
			return nullptr;
		}
		info.data_hex = src.data_hex;
	}
	if (info.size <= 0.0f || info.data_hex.empty())
	{
		warn_once(id, "无真字节登记");
		return nullptr;
	}

	std::vector<love::uint8> bytes = unhex(info.data_hex);
	if (bytes.empty())
	{
		warn_once(id, "hex 解码为空");
		return nullptr;
	}

	love::font::Font *fm =
		love::Module::getInstance<love::font::Font>(love::Module::M_FONT);
	if (fm == nullptr)
	{
		warn_once(id, "font 模块缺席");
		return nullptr;
	}

	// TrueTypeRasterizer 只吃 love::Data——FileData 承载拷贝（TrueType 数据
	// 无文件名语义，名字仅诊断用）。
	love::filesystem::FileData *data =
		new love::filesystem::FileData(bytes.size(), "fr_materialize_font.ttf");
	std::memcpy(data->getData(), bytes.data(), bytes.size());

	love::font::Rasterizer *rz = nullptr;
	try
	{
		// HINTING_NORMAL = wrap_Font 缺省口径（w_newTrueTypeRasterizer 同款）。
		rz = fm->newTrueTypeRasterizer(data, (int) (info.size + 0.5f),
		                               love::font::TrueTypeRasterizer::HINTING_NORMAL);
	}
	catch (love::Exception &)
	{
		data->release();
		warn_once(id, "newTrueTypeRasterizer 抛异常");
		return nullptr;
	}
	data->release();

	love::graphics::Font *font = nullptr;
	try
	{
		font = gfx->newFont(rz, gfx->getDefaultFilter());
	}
	catch (love::Exception &)
	{
		rz->release();
		warn_once(id, "newFont 抛异常");
		return nullptr;
	}
	rz->release();

	font_cache()[id] = love::StrongRef<love::graphics::Font>(font, love::Acquire::NORETAIN);
	return font;
}

love::graphics::Canvas *fr_materialize_canvas(love::graphics::Graphics *gfx, love::uint32 id)
{
	FrAssetInfo info;
	if (!fr_lookup_asset(id, info) || info.kind != FR_ASSET_CANVAS)
	{
		warn_once(id, "非 canvas 资产");
		return nullptr;
	}

	auto it = canvas_cache().find(id);
	if (it != canvas_cache().end())
		return it->second.get();

	love::graphics::Canvas::Settings settings;
	settings.width = info.w;
	settings.height = info.h;
	settings.dpiScale = 1.0f; // 像素域=逻辑域（_frNewCanvasReal 同口径）

	love::graphics::Canvas *canvas = nullptr;
	try
	{
		canvas = gfx->newCanvas(settings);
	}
	catch (love::Exception &)
	{
		warn_once(id, "newCanvas 抛异常");
		return nullptr;
	}

	canvas_cache()[id] =
		love::StrongRef<love::graphics::Canvas>(canvas, love::Acquire::NORETAIN);
	return canvas;
}

} // fr
