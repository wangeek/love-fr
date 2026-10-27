/**
 * FR 堆 CRC 档实现（B6 Task 1）——口径与红线见 fr_statediff.h。
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
// 每帧缓存 + 收集入口（present 钩 / _frStateCRC 同一遍历）
// ------------------------------------------------------------------

static std::vector<love::uint32> &scrcs()
{
	static std::vector<love::uint32> v;
	return v;
}

void capture_state_crc(lua_State *L)
{
	scrcs().push_back(fr_state_crc(L));
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
