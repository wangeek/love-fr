-- demo/kernel.lua —— 极简 demo 内核（B4-2 Task 3：恢复 L0 同款全帧四 op）。
-- draw 形参表按 shim 的 fr_ops 字段序（0-255 rgba8 IR 语义，适配层见 main.lua）：
--   clear(r, g, b, a) / draw(tex, quad, x, y, rot, sx, sy, ox, oy, r, g, b, a)
--   print(font, text, x, y, rot, sx, sy, r, g, b, a)
--   rectangle(mode, x, y, w, h, rx, ry, width, r, g, b, a)
-- 资产号按 main.lua 注册序：image=1、font=2（quad=0 = 空资产哨兵）。
-- 与 L0 golden 的差异（如实记录）：rectangle 的几何参数由 L0 的全屏 fill
-- (0,0,640,480) 改为底部红条 (0,456,640,24)——本 rig 验收是回放后 Canvas
-- 多点像素共存（背景黑/方块白/红条），全屏 fill 会盖掉方块与文字；行结构
-- 同构（rect 行仍在，mode=0 fill），仅几何参数不同，会话对拍锚为结构而非
-- 逐行（plan Task 4 口径）。
local kernel = { state = { tick = 0, hero = { x = 10.0, y = 20.0, hp = 87.0 }, banner = "L0" } }

function kernel.update(dt)
	local s = kernel.state
	s.tick = s.tick + 1
	s.hero.x = s.hero.x + 60.0 * dt
	if s.hero.x > 600 then s.hero.x = 600.0 end
end

function kernel.draw(g)
	g.clear(0, 0, 0, 255)
	g.draw(1, 0, kernel.state.hero.x, kernel.state.hero.y, 0, 1, 1, 0, 0, 255, 255, 255, 255)
	g.print(2, kernel.state.banner, 12.0, 340.0, 0, 1, 1, 255, 255, 0, 255)
	g.rectangle(0, 0.0, 456.0, 640.0, 24.0, 0.0, 0.0, 1.0, 255, 0, 0, 255)
end

-- 事件处理器行（run loop 分发直调）。处理器首参 em = runner 注入的
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
