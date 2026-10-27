-- demo/main.lua —— B4-2 Task 2 捕获 rig（fork demo，取代 Task 0 的 hint 骨架）。
-- 两种模式同一条路径，开关只改变 C++ 侧行为（行为等价回归）：
--   FR_CAPTURE=1   w_clear 抑制真执行只记行；present 钩回放行缓冲上屏；
--                  _frReplay 把行缓冲回放到验证 Canvas；_frTakeSession 有会话行。
--   FR_CAPTURE 未设 真 clear 直接执行；回放缓冲为空 no-op；会话行为空。
-- 自定义 love.run（替换默认循环——本批需要 present 钩时序可控，参照官方 11.5
-- love.run 骨架改）：pump → poll 丢弃（Task 2 无事件）→ update(dt) →
--   验证帧（frame N：切 Canvas → draw → _frReplay → getPixel 回读）→ draw → present。
-- 官方骨架的两处省略（rig 口径，实测记录）：
--   - 不做 love.graphics.clear(getBackgroundColor()) 预清屏——它会混入捕获会话行；
--   - 不做 isActive 守卫——rig 只跑 3 帧即退出，窗口最小化非目标场景。
local kernel = require("kernel")

-- IR 适配层：kernel 说 IR 语义（rgba8，0-255），翻译为 love.graphics 的 0-1 浮点。
local ir_graphics = {}
function ir_graphics.clear(r, g, b, a)
	love.graphics.clear(r / 255, g / 255, b / 255, a / 255)
end
-- draw/print/rectangle 适配：Task 3（语义替代四件套）接入。

function love.update(dt)
	kernel.update(dt)
end

function love.draw()
	kernel.draw(ir_graphics)
end

local VERIFY_FRAME = 3
local frame = 0
local dt = 0.0

-- 帧体（错误边界内）：Task 2 rig 的一切行为。
local function frame_body()
	if love.event then
		love.event.pump()
		for name in love.event.poll() do
			if name == "quit" then
				return 0
			end
			-- 其余事件 Task 2 丢弃（kernel.handlers 的接入归 Task 3）
		end
	end

	if love.timer then dt = love.timer.step() end
	if love.update then love.update(dt) end

	frame = frame + 1
	if frame >= VERIFY_FRAME then
		-- 自证（Canvas 主案）：render-to-texture 不依赖主屏交换，回读同步可靠。
		local c = love.graphics.newCanvas(64, 64)
		love.graphics.setCanvas(c)
		love.draw()                 -- 捕获模式：记录行（真执行被抑制）；关闭模式：真画
		love.graphics._frReplay()   -- 捕获模式：行缓冲回放到 canvas；关闭模式：空缓冲 no-op
		love.graphics.setCanvas()   -- 先解绑：active Canvas 不许 newImageData（11.5 实测报错）
		local img = c:newImageData()
		local r, g, b = img:getPixel(0, 0)
		-- getPixel 返回 0-1 浮点，×255 四舍五入打印整数（测试判据 PIXEL 1 0 0）
		print(string.format("PIXEL %d %d %d",
			math.floor(r * 255 + 0.5), math.floor(g * 255 + 0.5), math.floor(b * 255 + 0.5)))
		-- 会话行（测试 3）：捕获模式含 `clear 1 0 0 255`（每帧一条）
		local rows = love.graphics._frTakeSession()
		for _, row in ipairs(rows) do
			print(row)
		end
		io.stdout:flush()
		os.exit(0)
	end

	if love.graphics then
		love.draw()
		love.graphics.present()     -- 捕获模式：w_present 钩先回放后上屏
	end

	if love.timer then love.timer.sleep(0.001) end
end

function love.run()
	if love.timer then love.timer.step() end
	return function()
		-- 错误边界：帧内 Lua 错误 → stderr traceback + exit(1)，
		-- 不进 love 蓝色错误屏（测试 rig 不许交互挂住）。
		local ok, retval = xpcall(frame_body, debug.traceback)
		if not ok then
			io.stderr:write("fr-demo frame error:\n", tostring(retval), "\n")
			io.stderr:flush()
			os.exit(1)
		end
		return retval
	end
end
