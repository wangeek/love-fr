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
#include <sstream>
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

// B15 P1 增量编码状态
static bool s_delta = false;        // FR_DELTA=1
static love::uint64 s_delta_i = 32; // FR_DELTA_I（0=无限周期，仅 ev 强制 I）
static std::vector<std::string> s_ref_rows; // 上一个 I/写帧的行集（S/pd 基线）
static love::uint64 s_ref_frame = 0;        // S 引用的帧号

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
	static const char *kZeroSha = "0000000000000000000000000000000000000000000000000000000000000000";
	if (sha == nullptr || !valid_sha(sha))
	{
		// B11 拍板：缺省全 0 兜底（不再停用）——手动冒烟免填；判据对账
		//（fr-check 验表版本）需要真指纹时由调用方显式给。
		sha = kZeroSha;
		std::fprintf(stderr, "fr: FR_SHA 缺失/非法，会话头指纹记 0"
		             "（判据对账需真指纹时请显式传入）\n");
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

	// B15 P1：FR_DELTA=1 → 增量写出（head 标 +d，能力协商位）
	s_delta = getenv("FR_DELTA") != nullptr
	          && std::string(getenv("FR_DELTA")) == "1";
	if (const char *di = getenv("FR_DELTA_I"))
	{
		long v = strtol(di, nullptr, 10);
		if (v >= 0) s_delta_i = (love::uint64) v;
	}

	s_fh = std::fopen(out, "wb"); // canonical LF（"wb" 防 CRT 文本模式改写）
	if (s_fh == nullptr)
	{
		disable("文件打不开");
		return;
	}

	// 会话头（demo write_session 同构；几何 = 当前逻辑窗口尺寸）
	int w = gfx != nullptr ? gfx->getWidth() : 640;
	int h = gfx != nullptr ? gfx->getHeight() : 480;
	std::fprintf(s_fh, "frb1 %d %d %s %s %ld%s\n", w, h, sha,
	             s_full ? "full" : "base", seedv, s_delta ? " +d" : "");
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
	size_t ev_start = c_ev;
	for (; c_ev < ev.size(); c_ev++)
		std::fprintf(s_fh, "%s\n", ev[c_ev].c_str());
	bool cur_ev_frame = c_ev > ev_start; // B15：事件帧判定（I 帧强制条件）

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
	std::vector<std::string> cur_rows;
	for (; c_out < out.size(); c_out++)
	{
		if (!joined.empty())
			joined += "\n";
		joined += out[c_out];
		cur_rows.push_back(out[c_out]);
	}

	// B15 P1 增量编码（FR_DELTA=1 门控；head 已标 +d）。事件谓词先验 +
	// 行集 diff 断言 + I 帧策略（FR_DELTA_I 周期 / ev 帧强制）。
	bool wrote_same = false; // S 帧旗标（d_crc 语义分层）
	if (s_delta)
	{
		bool ev_frame = cur_ev_frame; // 本帧有 ev 行（已写出的 in 段）
		if (s_frame_no == 0 || ev_frame || (s_delta_i > 0 && s_frame_no % s_delta_i == 0))
		{
			// I 帧：全量行 + ref 基线更新
			for (const std::string &r : cur_rows)
				std::fprintf(s_fh, "%s\n", r.c_str());
			s_ref_rows = cur_rows;
			s_ref_frame = s_frame_no;
		}
		else if (cur_rows == s_ref_rows)
		{
			std::fprintf(s_fh, "S %llu\n", (unsigned long long) s_ref_frame);
			wrote_same = true;
		}
		else
		{
			// 行级 delta：朴素编辑脚本。**pos 语义 = 操作时刻的当前行序**；
			// 不变式：workpiece 前缀恒镜像 cur[0..b)——故任意块的操作位 = b
			//（删除全在 b 重复执行；插入从 b 递增）。LCS 跳同段；pd 优化后续精化。
			std::vector<std::string> rows = s_ref_rows; // 编辑工作件
			size_t a = 0, b = 0; // ref 游标 / cur 游标
			auto emit_del = [&](size_t pos)
			{
				std::fprintf(s_fh, "rdm %llu\n", (unsigned long long) pos);
				rows.erase(rows.begin() + pos);
			};
			auto emit_ins = [&](size_t pos, const std::string &line)
			{
				// 嵌套引号转义（print 行文本域含 " → \"）——解析端对称反转义
				std::string esc;
				for (char c : line)
				{
					if (c == '"' || c == '\\') esc += '\\';
					esc += c;
				}
				std::fprintf(s_fh, "rdp %llu \"%s\"\n",
				             (unsigned long long) pos, esc.c_str());
				rows.insert(rows.begin() + pos, line);
			};
			// pd 检测（B15 尾巴优化）：同身份行（首两 token 同=op+资产对）且
			// 恰一个数值 token 差 → pd 替代 rdp+rdm 对（单参绝对值行更短）。
			// 逐块替换处理：变更块内逐位配对（局部对位——回合制动画的行序
			// 稳定使然），配不上的位退回 rdp/rdm。
			auto try_pd = [&](const std::string &ref_line,
			                  const std::string &cur_line) -> bool
			{
				if (ref_line.compare(0, 5, "draw ") != 0)
					return false; // pd 白名单=draw 参数（rect 无资产身份）
				// token 化比差
				std::vector<std::string> rt, ct;
				{
					std::istringstream iss_r(ref_line), iss_c(cur_line);
					std::string t;
					while (iss_r >> t) rt.push_back(t);
					while (iss_c >> t) ct.push_back(t);
				}
				if (rt.size() != ct.size() || rt.size() < 3 || rt[0] != ct[0]
				    || rt[1] != ct[1])
					return false; // 长度变/op 变/资产对变 → 非 pd
				int diff = -1;
				for (size_t i = 2; i < rt.size(); i++)
					if (rt[i] != ct[i])
					{
						if (diff >= 0) return false; // 多 token 差
						diff = (int) i;
					}
				if (diff < 0) return false; // 全同（不该到这）
				// token 位 → 参数名（draw 签名序：tex quad x y rot sx sy ox oy r g b a）
				static const char *kNames[] = {"", "", "x", "y", "rot", "sx", "sy",
				                               "ox", "oy", "", "", "", ""};
				if (diff >= 2 && diff <= 9 && kNames[diff][0] != '\0')
				{
					unsigned asset = (unsigned) strtoul(ct[1].c_str(), nullptr, 10);
					// k = 帧内引用序（cur[0..b) 中该 asset 的出现数——workpiece
					// 前缀镜像 cur，直接数 cur 前缀）
					unsigned k = 0;
					for (size_t i = 0; i < b; i++)
					{
						unsigned ta = 0, qa = 0;
						if (sscanf(cur_rows[i].c_str(), "draw %u %u", &ta, &qa) == 2
						    && ta == asset)
							k++;
					}
					std::fprintf(s_fh, "pd %u %u %s %s\n", asset, k,
					             kNames[diff], ct[diff].c_str());
					rows[b] = cur_line; // 就地改写（workpiece 同步）
					return true;
				}
				return false; // 色差（rgb 位）→ rdp 全行（pd 白名单外）
			};
			while (a < s_ref_rows.size() || b < cur_rows.size())
			{
				if (a < s_ref_rows.size() && b < cur_rows.size()
				    && s_ref_rows[a] == cur_rows[b]) { a++; b++; continue; }
				// 块边界：跳到下一个重合点（短程跳查——行集局部性强；不中=到尾）
				size_t la = a, lb = b, skip = 0;
				while (la < s_ref_rows.size() && lb < cur_rows.size()
				       && s_ref_rows[la] != cur_rows[lb] && skip < 64) { la++; lb++; skip++; }
				if (!(skip < 64 && la < s_ref_rows.size() && lb < cur_rows.size()))
				{ la = s_ref_rows.size(); lb = cur_rows.size(); } // 尾块
				// pd 先试：块内逐位配对（等长块的同位行）
				size_t na = a, nb = b;
				bool used_pd = false;
				if (la - a == lb - b)
				{
					while (na < la && nb < lb)
					{
						if (try_pd(s_ref_rows[na], cur_rows[nb]))
						{
							na++; nb++; a++; b++; used_pd = true;
							continue;
						}
						break; // 首位配不上 → 整块退回编辑脚本（保 pos 不变式）
					}
				}
				if (used_pd && na == la && nb == lb)
					continue; // 整块 pd 化完成
				// 退回/剩余：编辑脚本（pd 已消费的游标已同步；剩余按块处理）
				for (size_t i = 0; i < la - a; i++) { emit_del(b); a++; }
				for (size_t i = 0; i < lb - b; i++) { emit_ins(b, cur_rows[b]); b++; }
				a = la; b = lb;
			}
			s_ref_rows = cur_rows;
			s_ref_frame = s_frame_no;
		}
	}
	else
	{
		for (const std::string &r : cur_rows)
			std::fprintf(s_fh, "%s\n", r.c_str());
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
	// B15 d_crc 语义分层：增量态 S 帧=锚声明（CRC("same:<ref>")）；其余
	//（I/delta/全量态）= 解码后行集 CRC（joined 即展开行集——writer 持全量）。
	if (wrote_same)
	{
		std::string decl = "same:" + std::to_string((unsigned long long) s_ref_frame);
		std::fprintf(s_fh, "d_crc %u\n",
		             (unsigned) fr_crc32_bytes(decl.data(), decl.size()));
	}
	else
	{
		std::fprintf(s_fh, "d_crc %u\n",
		             (unsigned) fr_crc32_bytes(joined.data(), joined.size()));
	}

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
