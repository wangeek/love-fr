/**
 * FR C 侧会话写出器实现（B10，设计见 fr_session.h）。
 **/

#include "modules/graphics/fr/fr_session.h"
#include "modules/graphics/fr/fr_capture.h"
#include "modules/graphics/fr/fr_statediff.h"
#include "modules/graphics/fr/fr_codec.h" // fr_crc32_bytes（d_crc 算法单一真源）

// LOVE
#include "common/int.h"
#include "modules/graphics/Graphics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace fr
{

static FILE *s_fh = nullptr;
static bool s_tried = false;     // env 解析一次（含禁用判定）
static bool s_full = false;      // FR_LOGLEVEL=full
static love::uint64 s_frame_no = 0;
static love::uint64 s_max_frames = 0; // 0 = 无限
static bool s_finished = false;

// 游标（全会话累积缓冲 → 当帧 delta；demo take_delta 同构）
static size_t c_out = 0, c_err = 0, c_ev = 0, c_scrc = 0;
static size_t c_sdiff_frame = 0; // sdiffs 是帧数组——按帧号索引

static double s_fallback_dt = 0.016667;
static bool s_dt_warned = false;

static void disable(const char *why)
{
	std::fprintf(stderr, "fr: FR_OUT 会话写出停用：%s\n", why);
	if (s_fh != nullptr)
	{
		std::fclose(s_fh);
		s_fh = nullptr;
	}
	s_tried = true;
}

static bool valid_sha(const char *s)
{
	if (strlen(s) != 64)
		return false;
	for (int i = 0; i < 64; i++)
	{
		char c = s[i];
		bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
		if (!ok)
			return false;
	}
	return true;
}

static void try_init(love::graphics::Graphics *gfx)
{
	s_tried = true;

	const char *out = getenv("FR_OUT");
	if (out == nullptr || out[0] == '\0')
		return; // 未启用（静默——常规路径）

	const char *sha = getenv("FR_SHA");
	if (sha == nullptr || !valid_sha(sha))
	{
		disable("FR_SHA 缺失或非 64 hex（会话头指纹）");
		return;
	}

	const char *seed = getenv("FR_SEED");
	long seedv = seed != nullptr ? strtol(seed, nullptr, 10) : 0;

	const char *frames = getenv("FR_FRAMES");
	if (frames != nullptr && frames[0] != '\0')
	{
		long n = strtol(frames, nullptr, 10);
		if (n > 0)
			s_max_frames = (love::uint64) n;
	}

	const char *fdt = getenv("FR_DT");
	if (fdt != nullptr && fdt[0] != '\0')
	{
		double v = strtod(fdt, nullptr);
		if (v > 0)
			s_fallback_dt = v;
	}

	s_full = getenv("FR_LOGLEVEL") != nullptr
	         && std::string(getenv("FR_LOGLEVEL")) == "full";

	s_fh = std::fopen(out, "wb"); // canonical LF（"wb" 防 CRT 文本模式改写）
	if (s_fh == nullptr)
	{
		disable("文件打不开");
		return;
	}

	// 会话头（demo write_session 同构；几何 = 当前逻辑窗口尺寸）
	int w = gfx != nullptr ? gfx->getWidth() : 640;
	int h = gfx != nullptr ? gfx->getHeight() : 480;
	std::fprintf(s_fh, "frb1 %d %d %s %s %ld\n", w, h, sha,
	             s_full ? "full" : "base", seedv);
	std::fprintf(s_fh, "cmnt fr fork c-tee session\n");
}

void fr_session_frame(love::graphics::Graphics *gfx)
{
	if (s_finished || !capture_enabled())
		return;
	if (!s_tried)
		try_init(gfx);
	if (s_fh == nullptr)
		return;
	if (s_max_frames != 0 && s_frame_no >= s_max_frames)
	{
		fr_session_finish(); // 到限封口（进程照常跑）
		return;
	}

	// 帧头
	std::fprintf(s_fh, "frame %llu\n", (unsigned long long) s_frame_no);

	// dt 行（w_step 钩事实源；未调 step 的自定义 run loop → FR_DT 兜底）
	double dt = 0.0;
	if (!fr_take_pending_dt(dt))
	{
		dt = s_fallback_dt;
		if (!s_dt_warned)
		{
			std::fprintf(stderr, "fr: 当帧无 timer.step 记录，dt 兜底 %.6f"
			             "（FR_DT 可调）\n", dt);
			s_dt_warned = true;
		}
	}
	std::fprintf(s_fh, "dt %.6f\n", dt);

	// asset 行（frame 0 checkpoint 注入口径）
	if (s_frame_no == 0)
	{
		for (const std::string &r : fr_take_asset_rows())
			std::fprintf(s_fh, "%s\n", r.c_str());
	}

	// ev 行（in 段）→ err 行（log 段，任何档）→ st 行（log 段，full 档）
	const std::vector<std::string> &ev = fr_view_ev_rows();
	for (; c_ev < ev.size(); c_ev++)
		std::fprintf(s_fh, "%s\n", ev[c_ev].c_str());

	const std::vector<std::string> &err = fr_view_err_rows();
	for (; c_err < err.size(); c_err++)
		std::fprintf(s_fh, "%s\n", err[c_err].c_str());

	if (s_full)
	{
		const std::vector<std::vector<std::string>> &sd = fr_view_sdiffs();
		if (c_sdiff_frame < sd.size())
		{
			for (const std::string &r : sd[c_sdiff_frame])
				std::fprintf(s_fh, "%s\n", r.c_str());
			c_sdiff_frame++;
		}
	}

	// out 行 delta + d_crc（当帧 out 行文本 join "\n" 的 CRC32——demo 同口径，
	// 空帧 join "" → CRC 0 如实记录）
	const std::vector<std::string> &out = fr_view_rows();
	std::string joined;
	for (; c_out < out.size(); c_out++)
	{
		if (!joined.empty())
			joined += "\n";
		joined += out[c_out];
		std::fprintf(s_fh, "%s\n", out[c_out].c_str());
	}

	// end 段：s_crc（present 钩先跑 capture_state_crc——每帧恰一份；缺则
	// 0 + 告警，如 FR_NO_SCRC 探针态）+ d_crc
	love::uint32 scrc = 0;
	const std::vector<love::uint32> &scrcs = fr_view_scrcs();
	if (c_scrc < scrcs.size())
		scrc = scrcs[c_scrc++];
	else if (!s_dt_warned)
	{
		std::fprintf(stderr, "fr: 当帧缺 s_crc 收集（NO_SCRC 探针态？），记 0\n");
		s_dt_warned = true; // 复用一次性旗标（同为诊断告警）
	}
	std::fprintf(s_fh, "s_crc %u\n", (unsigned) scrc);
	std::fprintf(s_fh, "d_crc %u\n", (unsigned) fr_crc32_bytes(joined.data(), joined.size()));

	fr_ev_reset_slot(); // 帧内 ev 取号序归零（下一帧从 0 起）
	s_frame_no++;
}

void fr_session_finish()
{
	if (s_finished)
		return;
	s_finished = true;
	if (s_fh != nullptr)
	{
		std::fflush(s_fh);
		std::fclose(s_fh);
		s_fh = nullptr;
		std::fprintf(stderr, "fr: FR_OUT 会话封口（%llu 帧）\n",
		             (unsigned long long) s_frame_no);
	}
}

} // namespace fr
