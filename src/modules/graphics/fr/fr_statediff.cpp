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

#include <cstdio>
#include <unordered_map>
#include <unordered_set>

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

} // fr
