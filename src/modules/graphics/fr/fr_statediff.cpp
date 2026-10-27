/**
 * FR 状态 CRC 档实现（B6 Task 1/1b）——口径与红线见 fr_statediff.h。
 *
 * Task 1b：s_crc 作用域 = 注册状态根可达子图（递归记忆化），未注册根回退
 * Task 1 全堆口径（gc.root 链序 CRC，实现保留于下）。
 *
 * include 路线（计划风险①的构建探针实证）：直 include LuaJIT 内部头
 * lj_obj.h——MEGA 构建里 love 目标 include 路径已含 LuaJIT/src
 * （MEGA_LUAJIT_INCLUDE，与 lua.h 同目录），lj_obj.h 只再依赖 lua.h/
 * lj_def.h/lj_arch.h（架构自检头，非构建期生成物），MSVC C++ TU 直编
 * 通过——未走"最小结构副本"退路。本 fork 构建为 x64 默认配置
 * （msvcbuild amalg，未传 nogc64）→ LJ_GC64=1（NaN 框 8 字节 TValue，
 * tag 在高 17 位：13 位 NaN 框 + 4 位 itype）——下方 static_assert 编译期
 * 实证；非 GC64 分支按 split tag（it 在 MSW）写全，条件宏自动择路。
 **/

#include "modules/graphics/fr/fr_statediff.h"
#include "modules/graphics/fr/fr_capture.h"

// Lua API（C 链接）先行：LuaJIT 的 lua.h 无 extern "C" 守卫，love 侧惯例
// 是 extern "C" 包裹引入（common/runtime.h:30 同款）——lj_obj.h 后续再
// include lua.h 时命中 include guard，声明保持 C 链接（否则 Lua API 符号
// 被 C++ 修饰，链接 lua51.lib 失败）。
extern "C" {
#include "lua.h"
}

// LuaJIT 内部结构（只读访问：G(L) / gc.root 链 / GCtab 槽位 / Node.val）。
#include "lj_obj.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fr
{

// 编译期对账：include 宏环境正确性的第一道证据（两种模式 TValue 均 8B：
// GC64=NaN 框 / 非 GC64=split tag；GC64 下 GCRef 为 64 位真指针装在高
// 47 位内）。布局漂移在此炸编译，而非运行时脏 CRC。
static_assert(sizeof(TValue) == 8, "LuaJIT TValue 应为 8 字节（GC64 NaN 框 / 非 GC64 split tag）");

// GC 载荷规范化（fr_statediff.h 口径）：指针位清零只留 tag——堆地址随
// ASLR 每进程漂移，原样计入则双跑确定性破。number 全位型、primitive
// （nil/true/false）原位型，二者本就无指针。
static uint64_t canonical_tv(const TValue *tv)
{
	if (tvisnumber(tv))
		return tv->u64;
	if (tvisgcv(tv) || tvislightud(tv))
	{
#if LJ_GC64
		// NaN 框：高 13 位全 1 + 4 位 itype，低 47 位为指针/整数载荷。
		return (uint64_t) tv->it64 & 0xFFFF800000000000ull;
#else
		// split tag（LE）：it 为 MSW（高 4 字节），载荷在低 4 字节。
		return tv->u64 & 0xFFFFFFFF00000000ull;
#endif
	}
	return tv->u64;
}

static inline void crc_mix_u32(love::uint32 &crc, love::uint32 v)
{
	crc = fr_crc32_update(crc, &v, sizeof(v));
}

static inline void crc_mix_u64(love::uint32 &crc, const TValue *tv)
{
	uint64_t canon = canonical_tv(tv);
	crc = fr_crc32_update(crc, &canon, sizeof(canon));
}

love::uint32 fr_state_crc(lua_State *L)
{
	global_State *g = G(L);
	love::uint32 crc = 0xFFFFFFFFu; // init（l0/walker.lua 同款 CRC32 状态机）
	love::uint32 ntab = 0;

	// gc.root 链遍历（分配序的逆）：只 GCtab（gct 过滤 = lj_gc.c 同款）。
	for (GCobj *o = gcref(g->gc.root); o != nullptr; o = gcnext(o))
	{
		if (o->gch.gct != (uint8_t) ~LJ_TTAB)
			continue;
		const GCtab *t = gco2tab(o);
		ntab++;
		crc_mix_u32(crc, t->asize);
		crc_mix_u32(crc, t->hmask);

		// 数组部：asize 个 TValue，nil 槽也计入（结构本身进 CRC）——
		// 下标语义明确，保序累加。
		const TValue *array = mref(t->array, const TValue);
		for (love::uint32 i = 0; i < t->asize; i++)
			crc_mix_u64(crc, &array[i]);

		// hash 部：hmask+1 个 Node，只 val TValue（key 不计）。**交换律
		// 累加（multiset 指纹），不按槽位线性序**——本 LuaJIT 的字符串
		// 哈希带每进程随机种子（StrInternState.seed "Random string seed"），
		// 字符串 key 落桶随进程漂移，线性序跨进程不可复现（实测取证，
		// 自审登记）；multiset 聚合下同键值集合 → 同贡献，值变化仍全检出
		//（每槽先取独立 digest 再求和，加法交换律保序无关）。空表
		// node = g->nilnode（hmask=0，val=nil），天然非空。
		const Node *node = noderef(t->node);
		if (node != nullptr)
		{
			love::uint32 acc = 0;
			for (love::uint32 i = 0; i <= t->hmask; i++)
			{
				uint64_t canon = canonical_tv(&node[i].val);
				acc += fr_crc32_update(0xFFFFFFFFu, &canon, sizeof(canon))
				       ^ 0xFFFFFFFFu;
			}
			crc_mix_u32(crc, acc);
		}
	}

	crc_mix_u32(crc, ntab); // 对象计数收尾混入（表数变化即改 CRC）
	return crc ^ 0xFFFFFFFFu; // xorout
}

// ------------------------------------------------------------------
// B6 Task 1b：注册状态根可达子图 CRC（D8 镜像范围裁定口径，序见 .h）
// ------------------------------------------------------------------

// Registry key（函数局部 static：地址唯一且必被取用，不会被内联丢弃）。
static void *state_root_key()
{
	static char key = 0;
	return &key;
}

void fr_state_root_set(lua_State *L, int idx)
{
	// Registry[key] = 栈上 idx 值：table 覆盖 / nil 清除（rawset 同键后写
	// 天然覆盖）。登记发生在 Lua 执行窗口内（w_frStateRoot 出口），rawset
	// 无元方法路径；root 表被内核持有，此强引用不改变 demo 生命周期语义。
	lua_pushlightuserdata(L, state_root_key());
	lua_pushvalue(L, idx);
	lua_rawset(L, LUA_REGISTRYINDEX);
}

namespace
{

// 子图遍历上下文：记忆化 + 环检测 + 结构计数（每次 fr_state_sub_crc 重建，
// Table* 地址只在单次遍历内作 map 键，跨进程不作比对量）。
struct SubWalk
{
	std::unordered_map<const GCtab *, love::uint32> memo; // 已完成表 → CRC
	std::unordered_set<const GCtab *> inprog;             // 递归栈上的表（环检测）
	love::uint32 ntab = 0;                                // 到达的不同表数
	bool cycle = false;
};

love::uint32 sub_table_crc(const GCtab *t, SubWalk &ctx)
{
	auto hit = ctx.memo.find(t);
	if (hit != ctx.memo.end())
		return hit->second; // 记忆化：同表多处引用只算一次，内容定 CRC
	if (!ctx.inprog.insert(t).second)
	{
		// 环引用（demo 无环，防御路径）：槽贡献按常数计，err 行在收尾记一次。
		ctx.cycle = true;
		return 0xC1C1E1E1u;
	}
	ctx.ntab++;

	love::uint32 crc = 0xFFFFFFFFu;
	crc_mix_u32(crc, t->asize);
	crc_mix_u32(crc, t->hmask);

	// 数组部（保序序列贡献）：table 槽在 canonical tag 位后追混子表递归
	// CRC（类型可辨 + 内容敏感两全）；其余槽 canonical 单混（number 全位
	// 型 / str-udata-func tag 位 / nil-bool 原位型）。
	const TValue *array = mref(t->array, const TValue);
	for (love::uint32 i = 0; i < t->asize; i++)
	{
		crc_mix_u64(crc, &array[i]);
		if (tvistab(&array[i]))
			crc_mix_u32(crc, sub_table_crc(tabV(&array[i]), ctx));
	}

	// hash 部（multiset 贡献）：每槽独立 digest 求和（加法交换律——字符串
	// 哈希种子每进程随机，线性序跨进程不可复现，与全堆口径同理）；table
	// 槽 digest 追混子表递归 CRC。空表 node = g->nilnode（hmask=0，val=nil）。
	const Node *node = noderef(t->node);
	if (node != nullptr)
	{
		love::uint32 acc = 0;
		for (love::uint32 i = 0; i <= t->hmask; i++)
		{
			const TValue *v = &node[i].val;
			uint64_t canon = canonical_tv(v);
			love::uint32 d = fr_crc32_update(0xFFFFFFFFu, &canon, sizeof(canon));
			if (tvistab(v))
			{
				love::uint32 cc = sub_table_crc(tabV(v), ctx);
				d = fr_crc32_update(d, &cc, sizeof(cc));
			}
			acc += d ^ 0xFFFFFFFFu;
		}
		crc_mix_u32(crc, acc);
	}

	ctx.inprog.erase(t);
	love::uint32 out = crc ^ 0xFFFFFFFFu;
	ctx.memo[t] = out;
	return out;
}

} // namespace

love::uint32 fr_state_sub_crc(lua_State *L)
{
	// 根取回：公共 API rawget（lightuserdata key，无分配无 GC 步进，红线
	// 不破），栈顶 TValue 直读出 GCtab*（lj_obj 宏，与全堆遍历同一路线）。
	lua_pushlightuserdata(L, state_root_key());
	lua_rawget(L, LUA_REGISTRYINDEX);
	const TValue *tv = L->top - 1;
	love::uint32 crc;
	if (tvistab(tv))
	{
		SubWalk ctx;
		crc = 0xFFFFFFFFu;
		crc_mix_u32(crc, sub_table_crc(tabV(tv), ctx));
		crc_mix_u32(crc, ctx.ntab); // 到达表数收尾混入（结构敏感性，全堆口径同款）
		crc ^= 0xFFFFFFFFu;
		if (ctx.cycle)
			fprintf(stderr, "fr_statediff: state root 子图含环（环槽按 0xC1C1E1E1 计，防御路径）\n");
	}
	else
		crc = fr_state_crc(L); // 未注册/非表根 → 全堆回退（从简裁定，无标记）
	lua_pop(L, 1);
	return crc;
}

// ------------------------------------------------------------------
// 每帧缓存 + 收集入口（present 钩 / _frStateSubCRC 同一遍历）
// ------------------------------------------------------------------

static std::vector<love::uint32> &scrcs()
{
	static std::vector<love::uint32> v;
	return v;
}

void capture_state_crc(lua_State *L)
{
	// Task 1b 口径：根已注册 → 子图 CRC；否则全堆回退（dispatch 在
	// fr_state_sub_crc 内，与显式出口 _frStateSubCRC 同一取值路径）。
	scrcs().push_back(fr_state_sub_crc(L));
}

int w_frStateRoot(lua_State *L)
{
	// love.graphics._frStateRoot(t)：t=table 登记状态根（后者覆盖）/
	// nil 清除；其余类型拒绝（登记口在 Lua 执行窗口内，可安全报错）。
	if (!lua_istable(L, 1) && !lua_isnil(L, 1))
	{
		lua_pushliteral(L, "fr_statediff: _frStateRoot expects table or nil");
		return lua_error(L);
	}
	fr_state_root_set(L, 1);
	return 0;
}

int w_frStateSubCRC(lua_State *L)
{
	love::uint32 crc = fr_state_sub_crc(L);
	scrcs().push_back(crc);
	lua_pushinteger(L, (lua_Integer) crc);
	return 1;
}

int w_frStateCRC(lua_State *L)
{
	love::uint32 crc = fr_state_crc(L);
	scrcs().push_back(crc);
	lua_pushinteger(L, (lua_Integer) crc);
	return 1;
}

int w_frTakeSCrcs(lua_State *L)
{
	lua_createtable(L, (int) scrcs().size(), 0);
	for (size_t i = 0; i < scrcs().size(); i++)
	{
		lua_pushinteger(L, (lua_Integer) scrcs()[i]);
		lua_rawseti(L, -2, (int) i + 1);
	}
	return 1;
}

// ------------------------------------------------------------------
// B6 Task 2：diff 档——影子镜像 + st 物理行 + 表重建（FR_LOGLEVEL=full）
// ------------------------------------------------------------------

bool state_diff_full()
{
	static const bool full = []()
	{
		const char *v = getenv("FR_LOGLEVEL");
		return v != nullptr && std::string(v) == "full";
	}();
	return full;
}

namespace
{

// 影子镜像（statediff 自有内存，堆外——不算写 VM，只读红线不破）：
// 数组部按下标存槽指纹、hash 部按 key canonical 串寻址（Node 落桶随插入/
// 搬移漂移——Brent 变体 newkey 可在 rehash 外移动条目，lj_tab.c:458-507，
// 按下标镜像会假阳性，键寻址才稳）。
//
// 槽指纹（diff 档专用）：canonical_tv 基础上对 str 槽补内容 CRC——CRC 档
// 口径 str 只计 tag 位（Task 1，载荷指针位清零保双跑确定），但 diff 档的
// 物理行要逮字符串值变化（banner "L0"→"KEY"，实测曾整段漏发）；内容
//（len+bytes）跨进程确定，指针位仍不进指纹。table 引用槽维持 tag 位
//（子表内容由子表自身镜像覆盖；s_crc 的 str 内容盲区同为 Task 1 既定
// 口径，登记自审，本批不动 CRC 语义）。
struct SlotFp
{
	uint64_t canon = 0;      // canonical_tv 位型（number 全位型 / GC 引用 tag 位）
	love::uint32 strc = 0;   // str 槽内容 CRC32（0=非 str）
	bool operator!=(const SlotFp &o) const { return canon != o.canon || strc != o.strc; }
};

static SlotFp slot_fp(const TValue *tv)
{
	SlotFp fp;
	fp.canon = canonical_tv(tv);
	if (tvisstr(tv))
	{
		const GCstr *s = strV(tv);
		love::uint32 crc = 0xFFFFFFFFu;
		crc = fr_crc32_update(crc, &fp.canon, sizeof(fp.canon));
		crc = fr_crc32_update(crc, &s->len, sizeof(s->len));
		crc = fr_crc32_update(crc, strdata(s), s->len);
		fp.strc = crc ^ 0xFFFFFFFFu;
	}
	return fp;
}

struct TableMirror
{
	love::uint32 asize = 0;
	love::uint32 hmask = 0;
	std::vector<SlotFp> arr;                // 数组部槽指纹（下标对位）
	std::map<std::string, SlotFp> hash;     // 占用 hash 槽：key canonical 串 → 槽指纹
};

// 对象 id/弱校验登记（跨帧持久；镜像释放后条目保留——同址再现弱校验用）。
struct ObjMeta
{
	love::uint32 id = 0;
	love::uint32 asize = 0;
	love::uint32 hmask = 0;
	love::uint32 fp = 0;
};

std::unordered_map<const GCtab *, TableMirror> &mirror_map()
{
	static std::unordered_map<const GCtab *, TableMirror> m;
	return m;
}

std::unordered_map<const GCtab *, ObjMeta> &id_map()
{
	static std::unordered_map<const GCtab *, ObjMeta> m;
	return m;
}

love::uint32 next_obj_id()
{
	static love::uint32 id = 0;
	return ++id; // 单调递增不复用（epoch 语义）
}

// 弱校验指纹（Table* 复用检测，确定性——跨进程可比）：asize>0 → 首数组槽
// 指纹；否则 hash 部 multiset 累加（CRC 口径同款，槽序无关）。
love::uint32 table_fp(const GCtab *t)
{
	if (t->asize > 0)
	{
		SlotFp fp = slot_fp(&mref(t->array, const TValue)[0]);
		return fr_crc32_update(0xFFFFFFFFu, &fp, sizeof(fp)) ^ 0xFFFFFFFFu;
	}
	love::uint32 acc = 0;
	const Node *node = noderef(t->node);
	if (node != nullptr)
		for (love::uint32 i = 0; i <= t->hmask; i++)
		{
			SlotFp fp = slot_fp(&node[i].val);
			acc += fr_crc32_update(0xFFFFFFFFu, &fp, sizeof(fp)) ^ 0xFFFFFFFFu;
		}
	return acc;
}

// TValue tag 名（err 行登记用）。
static const char *tv_tagname(const TValue *tv)
{
	if (tvisnil(tv)) return "nil";
	if (tvistrue(tv) || tvisfalse(tv)) return "bool";
	if (tvisnumber(tv)) return "num";
	if (tvisstr(tv)) return "str";
	if (tvistab(tv)) return "tab";
	if (tvisudata(tv)) return "udata";
	if (tvisfunc(tv)) return "func";
	if (tvislightud(tv)) return "lightud";
	if (tviscdata(tv)) return "cdata";
	return "other";
}

// st 行 variant 渲染（cmd.tbl val variant 域：num %.6f / str "…" / bool 0|1，
// 与 gen_python _render_variant 同构）。可序列化 → true；nil/table/… → false。
// NaN/Inf 防御拒发（%.6f 容不下，demo 无——错误侧如实暴露）。
bool render_variant(const TValue *tv, std::string &out)
{
	if (tvisnumber(tv))
	{
		double n = numV(tv);
		if (n != n || n * 0.0 != 0.0)
			return false;
		char buf[64];
		snprintf(buf, sizeof(buf), "num %.6f", n);
		out = buf;
		return true;
	}
	if (tvisstr(tv))
	{
		const GCstr *s = strV(tv);
		out = "str \"" + fr_esc_text(std::string(strdata(s), s->len)) + "\"";
		return true;
	}
	if (tvistrue(tv) || tvisfalse(tv))
	{
		out = tvistrue(tv) ? "bool 1" : "bool 0";
		return true;
	}
	return false;
}

// hash 槽 key canonical 串（镜像键与行内 key 段共用）：字符串 key 用原串、
// 数字 key "num:%.6f" 前缀记法（拍板）、bool "bool:0|1"；其余 key 类型
// （tab/func/lightud/cdata，demo 无）→ false（槽不可寻址，err 防御登记）。
// 已知边界：字符串 key 与数字 key 的 canonical 串理论可撞（如字面量
// "num:3.000000"）——demo 无此形态，镜像合槽如实登记。
bool render_slotkey(const TValue *key, std::string &out)
{
	if (tvisstr(key))
	{
		const GCstr *s = strV(key);
		out.assign(strdata(s), s->len);
		return true;
	}
	if (tvisnumber(key))
	{
		double n = numV(key);
		if (n != n || n * 0.0 != 0.0)
			return false;
		char buf[64];
		snprintf(buf, sizeof(buf), "num:%.6f", n);
		out = buf;
		return true;
	}
	if (tvistrue(key) || tvisfalse(key))
	{
		out = tvistrue(key) ? "bool:1" : "bool:0";
		return true;
	}
	return false;
}

// 值域五类外的槽 → err 行（cmd.tbl err：what str 单串，"表重建计数/忠实性
// 事实"语义位；拍板行形 st-unsupported <id> <slot> <tag> 收进 what 串——
// cmd.tbl 为准，行内三 token 形与 err 签名不容）。nil 槽不产行：variant 域
// 无 nil（Lua 语义 nil=键缺席），值保真由 s_crc 全位型承担，如实登记。
void emit_slot_row(std::vector<std::string> &rows, love::uint32 id,
                   const std::string &key, const TValue *val)
{
	std::string var;
	if (render_variant(val, var))
		rows.push_back("st " + std::to_string(id) + " \""
		               + fr_esc_text(key) + "\" " + var);
	else if (!tvisnil(val))
		rows.push_back("err \"st-unsupported " + std::to_string(id) + " "
		               + fr_esc_text(key) + " " + tv_tagname(val) + "\"");
}

struct DiffWalk
{
	std::vector<std::string> rows;
	std::unordered_set<const GCtab *> visited;  // 本帧已到（memo/环终止）
	std::unordered_set<const GCtab *> live;     // 本帧子图内（镜像释放判定）
};

struct SlotRef
{
	const TValue *val;
	std::string key;
};

// 单表 diff（自递归入口）。行序确定性：本表槽行在前（数组部 0 基下标序 +
// hash 部 key canonical 串字节序）、子表递归在后（同槽序）；表重建
//（首见/asize|hmask 变）→ err st-rehash 行（cmd.tbl err"表重建"语义位）+
// 全槽重发，不逐槽比。
void diff_table(const GCtab *t, DiffWalk &ctx)
{
	if (!ctx.visited.insert(t).second)
		return; // memo：同表多处引用只处理一次（环在此终止；引用槽的 err 行已由父槽记录）
	ctx.live.insert(t);

	// 对象 id 解析（跨帧持久，id 单调不复用；Table* 复用弱校验=epoch 最简实现，
	// demo 深度不触发，防御登记）：无登记 → 新 id；有登记且镜像在 → 续用；
	// 有登记而镜像已释放（地址再现）→ (asize,hmask,指纹) 过 → 视同表续 id，
	// 不过 → 同址异表，换新 id。
	love::uint32 id;
	bool full_send;
	auto &ids = id_map();
	auto iit = ids.find(t);
	if (iit == ids.end())
	{
		id = next_obj_id();
		ObjMeta m;
		m.id = id;
		m.asize = t->asize;
		m.hmask = t->hmask;
		m.fp = table_fp(t);
		ids[t] = m;
		full_send = true;
	}
	else
	{
		id = iit->second.id;
		if (mirror_map().count(t) != 0)
			full_send = false;
		else
		{
			love::uint32 fp = table_fp(t);
			if (iit->second.asize == t->asize && iit->second.hmask == t->hmask
			    && iit->second.fp == fp)
			{
				full_send = true; // 同对象回归（子图外一帧后重现）：保 id 全发重续
			}
			else
			{
				id = next_obj_id(); // 同址异表（GC 复用地址）：换新 id，登记换新
				ObjMeta m;
				m.id = id;
				m.asize = t->asize;
				m.hmask = t->hmask;
				m.fp = fp;
				ids[t] = m;
				full_send = true;
			}
		}
	}

	auto &mirrors = mirror_map();
	auto mit = mirrors.find(t);
	bool structural = mit == mirrors.end()
	    || mit->second.asize != t->asize || mit->second.hmask != t->hmask;
	if (structural && mit != mirrors.end())
	{
		// 表重建行（cmd.tbl err"表重建计数"语义位；asize/hmask 值域 u32×2
		// 进 what 串——err what str 单串签名，cmd.tbl 为准）。
		ctx.rows.push_back("err \"st-rehash " + std::to_string(id) + " "
		                   + std::to_string(t->asize) + " "
		                   + std::to_string(t->hmask) + "\"");
	}

	// 占用槽收集：数组部（下标 → "[i]"，0 基）保序；hash 部占用槽（val 非
	// nil）按 key canonical 串排序。不可寻址 key → err 防御行、槽跳过。
	std::vector<SlotRef> slots;
	const TValue *array = mref(t->array, const TValue);
	for (love::uint32 i = 0; i < t->asize; i++)
	{
		if (tvisnil(&array[i]))
			continue;
		slots.push_back({ &array[i], "[" + std::to_string(i) + "]" });
	}
	const Node *node = noderef(t->node);
	std::vector<SlotRef> hslots;
	if (node != nullptr)
		for (love::uint32 i = 0; i <= t->hmask; i++)
		{
			const Node *n = &node[i];
			if (tvisnil(&n->val))
				continue; // 空/死槽（死键 val=nil，lj_tab 无 tombstone 遍历语义）
			std::string key;
			if (!render_slotkey(&n->key, key))
			{
				ctx.rows.push_back("err \"st-unsupported " + std::to_string(id)
				                   + " key " + tv_tagname(&n->key) + "\"");
				continue;
			}
			hslots.push_back({ &n->val, key });
		}
	std::sort(hslots.begin(), hslots.end(),
	          [](const SlotRef &a, const SlotRef &b) { return a.key < b.key; });
	slots.insert(slots.end(), hslots.begin(), hslots.end());

	if (full_send || structural)
	{
		// 全槽重发（首见/表重建）：占用槽逐槽产行（含 err unsupported）。
		for (const SlotRef &s : slots)
			emit_slot_row(ctx.rows, id, s.key, s.val);
		// 镜像整建。
		TableMirror nm;
		nm.asize = t->asize;
		nm.hmask = t->hmask;
		for (love::uint32 i = 0; i < t->asize; i++)
			nm.arr.push_back(slot_fp(&array[i]));
		for (const SlotRef &s : hslots)
			nm.hash[s.key] = slot_fp(s.val);
		mirrors[t] = std::move(nm);
	}
	else
	{
		TableMirror &m = mit->second;
		// 数组部逐槽指纹比对（asize 未变 → 尺寸对位）：脏槽产行（nil 新值
		// 不产行——变 nil = 槽缺席，镜像仍记 nil 指纹，复占再检出）。
		// 数组部下标 = 整数键原值（lj_tab newtab 注释/bestasize 的 +1：
		// 1-based Lua 数组空置 [0]，clicks[1] 物理落 [1]——实测定形）。
		for (love::uint32 i = 0; i < t->asize; i++)
		{
			SlotFp fp = slot_fp(&array[i]);
			if (fp != m.arr[i])
				emit_slot_row(ctx.rows, id, "[" + std::to_string(i) + "]", &array[i]);
			m.arr[i] = fp;
		}
		// hash 部按 key 寻址比对；整表重建（消失键随重建出镜像——变 nil 不
		// 产行，s_crc 全位型承担值保真）。
		std::map<std::string, SlotFp> livehash;
		for (const SlotRef &s : hslots)
		{
			SlotFp fp = slot_fp(s.val);
			auto hit = m.hash.find(s.key);
			if (hit == m.hash.end() || hit->second != fp)
				emit_slot_row(ctx.rows, id, s.key, s.val);
			livehash[s.key] = fp;
		}
		m.hash = std::move(livehash);
	}

	// 子表递归（table 引用槽的 err unsupported 行已在上面随槽发行）。
	for (const SlotRef &s : slots)
		if (tvistab(s.val))
			diff_table(tabV(s.val), ctx);
}

} // namespace

std::vector<std::string> fr_state_diff_rows(lua_State *L)
{
	std::vector<std::string> rows;
	// 根取回与 fr_state_sub_crc 同根同法（公共 API rawget，无分配无 GC 步进）。
	lua_pushlightuserdata(L, state_root_key());
	lua_rawget(L, LUA_REGISTRYINDEX);
	const TValue *tv = L->top - 1;
	if (tvistab(tv))
	{
		DiffWalk ctx;
		diff_table(tabV(tv), ctx);
		// 镜像释放：本帧子图外的条目（表离开状态子图/被 GC）——不发行
		//（消失不是状态变更，s_crc 天然反映）；id 登记保留（同址再现弱校验）。
		auto &mirrors = mirror_map();
		for (auto it = mirrors.begin(); it != mirrors.end(); )
		{
			if (ctx.live.count(it->first) == 0)
				it = mirrors.erase(it);
			else
				++it;
		}
		rows = std::move(ctx.rows);
	}
	else
		fprintf(stderr, "fr_statediff: 未注册状态根，diff 档无行（全堆口径不产 diff 行）\n");
	lua_pop(L, 1);
	return rows;
}

static std::vector<std::vector<std::string>> &sdiffs()
{
	static std::vector<std::vector<std::string>> v;
	return v;
}

void capture_state_diff(lua_State *L)
{
	// full 级档位开关（base/off 不收集——档位隔离）；收集点与 capture_state_crc
	// 同一 present 帧界，每帧恰一份（验证帧/--sim 的显式出口同纪律）。
	if (!state_diff_full())
		return;
	sdiffs().push_back(fr_state_diff_rows(L));
}

static int push_string_array(lua_State *L, const std::vector<std::string> &v)
{
	lua_createtable(L, (int) v.size(), 0);
	for (size_t i = 0; i < v.size(); i++)
	{
		const std::string &s = v[i];
		lua_pushlstring(L, s.data(), s.size());
		lua_rawseti(L, -2, (int) i + 1);
	}
	return 1;
}

int w_frStateDiffRows(lua_State *L)
{
	std::vector<std::string> rows = fr_state_diff_rows(L);
	sdiffs().push_back(rows); // 当帧缓存（与 s_crc 出口同帧纪律）
	return push_string_array(L, rows);
}

int w_frTakeSDiffRows(lua_State *L)
{
	// 帧数组的数组：sdiffs[f] = 第 f 帧收集的 log 段行（收集序 = 帧序）。
	lua_createtable(L, (int) sdiffs().size(), 0);
	for (size_t f = 0; f < sdiffs().size(); f++)
	{
		push_string_array(L, sdiffs()[f]);
		lua_rawseti(L, -2, (int) f + 1);
	}
	return 1;
}

} // fr
