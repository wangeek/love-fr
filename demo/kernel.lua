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
-- B5 Task 1 事件驱动化：state.clicks（最多 5 个点击点）+ banner 随 keypressed
-- 切换——out 行随事件可变，重演比对才有因果锚。
local kernel = { state = { tick = 0, hero = { x = 10.0, y = 20.0, hp = 87.0 }, banner = "L0", clicks = {} } }

function kernel.update(dt)
	local s = kernel.state
	s.tick = s.tick + 1
	s.hero.x = s.hero.x + 60.0 * dt
	if s.hero.x > 600 then s.hero.x = 600.0 end
end

function kernel.draw(g, assets)
	g.clear(0, 0, 0, 255)
	g.draw(1, 0, kernel.state.hero.x, kernel.state.hero.y, 0, 1, 1, 0, 0, 255, 255, 255, 255)
	g.print(2, kernel.state.banner, 12.0, 340.0, 0, 1, 1, 255, 255, 0, 255)
	g.rectangle(0, 0.0, 456.0, 640.0, 24.0, 0.0, 0.0, 1.0, 255, 0, 0, 255)
	-- 点击标记：每个 click 画 8x8 白 rect（中心=点击点，左上 (x-4, y-4)）。
	-- 画在最后（红条之上）——验证帧三点像素锚 (400,100)/(20,30)/(320,468)
	-- 不与标记区重叠，注入路径不破既有像素判据。
	for _, c in ipairs(kernel.state.clicks) do
		g.rectangle(0, c.x - 4.0, c.y - 4.0, 8.0, 8.0, 0.0, 0.0, 1.0, 255, 255, 255, 255)
	end

	-- B8 T1 原生 API 序列（真实游戏形态，不经 ir_graphics 适配层）：
	-- setColor/setLineWidth 状态折参（真状态机）+ setFont 状态跟踪 + print
	-- 不传 font。几何避开验证帧三点像素锚 (400,100)/(20,30)/(320,468)。
	if assets then
		love.graphics.setFont(assets[2])          -- 代理 → fr 状态记 id=2
		love.graphics.setColor(0, 1, 0, 1)          -- 绿（0-1 域，原生语义）
		love.graphics.print("N8", 500.0, 10.0)     -- 不传 font → setFont 状态
		love.graphics.setLineWidth(3)
		love.graphics.rectangle("line", 500.0, 200.0, 40.0, 20.0)
		love.graphics.setColor(1, 1, 1, 1)          -- 复位（后续帧点击标记仍白）

		-- B8 T2 绘制类五 op（原生调用，坐标避验证帧三点像素锚）
		love.graphics.setColor(0, 1, 1, 1)          -- 青
		love.graphics.line(80.0, 60.0, 120.0, 90.0)
		love.graphics.setLineWidth(2)
		love.graphics.line(80.0, 60.0, 120.0, 90.0) -- 青线宽 2
		love.graphics.setColor(1, 0, 0, 1)          -- 红
		love.graphics.circle("fill", 600.0, 400.0, 15.0, 32)
		love.graphics.setColor(1, 1, 0, 1)          -- 黄
		love.graphics.polygon("fill", 40.0, 100.0, 60.0, 140.0, 20.0, 140.0)
		love.graphics.setColor(0, 1, 1, 1)          -- 青
		love.graphics.arc("fill", "pie", 300.0, 200.0, 25.0, 0.0, 1.570796, 16)
		love.graphics.setColor(1, 1, 1, 1)          -- 复位
	end
end

-- 事件处理器（B5 Task 1）：main.lua run loop 的 poll 快照逐条分发直调，
-- 签名 = love 同名回调的参数前缀（kernel 只取关心的前缀参，尾参可略）。
kernel.handlers = {
	-- 点击记账：state.clicks 追加 {x, y}，最多 5 个（超出移除最旧）
	mousepressed = function(x, y, button)
		local clicks = kernel.state.clicks
		clicks[#clicks + 1] = { x = x, y = y }
		if #clicks > 5 then table.remove(clicks, 1) end
	end,
	-- banner 在 "L0"/"KEY" 间切换（toggle）——print 行随事件可变
	keypressed = function(key)
		kernel.state.banner = (kernel.state.banner == "L0") and "KEY" or "L0"
	end,
}

return kernel
