/**
 * FR 真纹理物化器（B4 阶段三，手写层）。
 *
 * 职责：回放侧按资产 id 把 fr_capture 登记的 CPU 字节物化为真 GPU 对象——
 * image：hex → bytes → love::image::ImageData（newImageData(w,h,RGBA8,data)
 * 拷贝构造，w_newImage 同链）→ Image::Slices(TEXTURE_2D) → gfx->newImage
 * （Graphics.h:474 签名）；font：hex → bytes → FileData 承载 →
 * love::font::Font::newTrueTypeRasterizer(data, size, HINTING_NORMAL) →
 * gfx->newFont(rasterizer, getDefaultFilter())（w_newFont/w_newTrueTypeRasterizer
 * 同链——newFont(size) 的默认字体即 Font.cpp 内嵌 Vera.ttf，登记字节与重建
 * 路径同源）。
 *
 * 缓存：id → StrongRef 全会话缓存，物化一次、replay 幂等（行缓冲不消费，
 * present 钩每帧重放全量行——不缓存会重复上传 GPU 资产）。返回指针归缓存
 * 所有，调用方不得 release。
 *
 * 失败面（返回 nullptr，调用方回退占位渲染）：id 未登记 / kind 不符 /
 * 字节缺失或长度不符（RGBA8 域 = w*h*4）/ 模块缺席 / 构造抛 love::Exception
 * （物化器内部吞异常转告警——capture_replay 无 Lua 边界，异常外溢即
 * terminate）。stderr 告警按 id 一次性（warned 集）。
 *
 * image 字节口径 = RGBA8（协议约定，见 fr_capture.h FrAssetInfo 注）；
 * font 物化固定 HINTING_NORMAL（wrap_Font 缺省同口径，dpiscale 不走）。
 **/

#pragma once

// LOVE
#include "common/config.h"
#include "common/int.h"

namespace love
{
namespace graphics
{
class Graphics;
class Image;
class Font;
} // graphics
} // love

namespace fr
{

// 登记资产 id → 真 Image（缓存所有，nullptr = 不可物化，见文件头失败面）。
love::graphics::Image *fr_materialize_image(love::graphics::Graphics *gfx, love::uint32 id);

// 登记资产 id → 真 Font（缓存所有，nullptr = 不可物化）。
love::graphics::Font *fr_materialize_font(love::graphics::Graphics *gfx, love::uint32 id);

} // fr
