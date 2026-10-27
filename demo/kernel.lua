-- demo/kernel.lua —— 极简 demo 内核（B4-2 Task 2 口径：draw 收窄为单一 clear，
-- clear 端到端验证用；Task 3 全帧捕获时自 l0/kernel.lua 恢复 L0 同款画面并接入
-- 状态走查器）。
-- draw 形参表仍按 shim 的 fr_ops 字段序（0-255 rgba8 IR 语义，适配层见 main.lua）：
--   clear(r, g, b, a) / draw(tex, quad, x, y, ...) / print(font, text, x, y, ...)
--   rectangle(mode, x, y, w, h, rx, ry, width, r, g, b, a)
-- 资产号按 main.lua 注册序：image=1、font=2（quad=0 = 空资产哨兵）。
local kernel = { state = { tick = 0, hero = { x = 10.0, y = 20.0, hp = 87.0 }, banner = "L0" } }

function kernel.update(dt)
	local s = kernel.state
	s.tick = s.tick + 1
	s.hero.x = s.hero.x + 60.0 * dt
	if s.hero.x > 600 then s.hero.x = 600.0 end
end

function kernel.draw(g)
	-- Task 2：只画红 clear（`clear 1 0 0 255`，rgba8）——capture-then-replay 端到端。
	g.clear(1, 0, 0, 255)
end

-- Task 3：事件处理器行（run loop 分发直调）。处理器首参 em = runner 注入的
-- CALL 行记录器：em(fn, depth, raw_args) 记同步直调（slot 取帧内 in 段流水号）。
function kernel.on_key(key)
	-- banner 可被 walker st 行观察——CALL 链的状态效果由此进入 log 段
	kernel.state.banner = "KEY:" .. key
end

kernel.handlers = {
	-- 演示一层 CALL：keypressed 直调另一处理器 on_key（CALL 行 depth=1）
	keypressed = function(em, key, scancode, isrepeat)
		em("on_key", 1, { key, scancode, isrepeat })
		kernel.on_key(key)
	end,
	-- quit 无处理器：runner 视 quit 事件在本帧闭合后终止循环
}

return kernel
