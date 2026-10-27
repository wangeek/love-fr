-- demo/main.lua —— B4-2 Task 4 捕获 rig（fork demo：Task 3 全帧捕获 + tee₁ 会话写出）。
-- 两种模式同一条路径，开关只改变 C++ 侧行为（行为等价回归）：
--   FR_CAPTURE=1   w_clear/draw/print/rectangle 抑制真执行只记行；构造四件套
--                  （newImage/newFont/newCanvas/newQuad）走语义替代（资产登记
--                  + 代理）；present 钩回放行缓冲上屏；_frReplay 把行缓冲回放
--                  到验证 Canvas；_frTakeSession/_frTakeAssets 有会话行。
--   FR_CAPTURE 未设 真 clear/draw/print/rectangle 直接执行；回放缓冲为空
--                  no-op；会话缓冲为空。
-- 离线回放（B4 阶段三 Task 3）：--offline-replay <in.fr|.frb> 独占——不跑
--                  kernel/run loop，C 入口读流回放采集 D_CRC 后直接退出。
-- 资产等价基准（B4 阶段三口径）：红方块两版同色——捕获版 draw 回放经真纹理
-- 物化（登记 ImageData 字节 → Image），真版 Image 由同一份红色 ImageData 建
-- 真纹理。红条/背景为无资产 op，天然两版同色。
-- 自定义 love.run（Task 2 骨架沿用）：pump → poll 捕获 ev 行/分发 handlers
--   （B5 Task 1，此前为丢弃）→ update(dt) → 验证帧（frame 3：真 Canvas
--   （_frNewCanvasReal，开关无关）→ draw → _frReplay → 三点 getPixel 回读）
--   → draw → present。
-- 官方骨架的两处省略（rig 口径，Task 2 实测记录）：
--   - 不做 love.graphics.clear(getBackgroundColor()) 预清屏——它会混入捕获会话行；
--   - 不做 isActive 守卫——rig 只跑数帧即退出，窗口最小化非目标场景。
-- dt 固定 0.016667（L0 golden 同源），hero.x 帧序 11.000020/12.000040/13.000060
-- （10 + 60*dt*k，qf 域复算）——真实墙钟时间不定，会话行坐标不可锚定。
--
-- tee₁ 会话写出（Task 4）：--out <path> 存在即启用 tee 模式——会话末组装 .fr
-- 文本写文件（UTF-8/LF，末尾单 LF）。组装口径复用 l0/main.lua 的成熟结构：
-- 头行 `frb1 640 480 <sha> base <seed>` + cmnt 固定文本 + 帧块（帧内行序
-- dt → 资产（仅 frame 0，checkpoint 注入口径）→ in 事件行 → out 捕获行）。
-- C 捕获层当前不产 st/s_crc/d_crc 行（会话级校验归 B6 堆遍历）——**本会话为
-- out-only 形态，end 段缺省**。参数：--frames N（缺省 4）、--sha <64hex>
--（cmd.tbl 指纹，调用侧算好传入；缺/坏报错 exit 2，L0 同款）、--seed n
--（缺省 0）。确定性：FIXED_DT 固定 + cmnt 固定 + 无随机 → 两次运行逐字节
-- 一致（测试锚）。验证帧（frame 3）的 Canvas 回读与 tee 并存：先验证后 tee。
--
-- B5 Task 1（事件捕获 + 事件驱动化）：poll 循环不再丢弃——白名单内离散输入
-- 事件逐条记 ev 行进当帧 in 缓冲（Lua 侧拼行，格式源对齐 l0/fr.lua 的
-- esc/f/tok_variant）+ 分发 kernel.handlers（事件驱动 out 行）。ev 行形
-- `ev <slot> "<name>" <src> <argc> <variant...>`（fr-spec §6.2），src=0
--（poll 取出即 OS 路径——注入的 push 落同一队列同路径取出）。环境/连续
-- 事件（focus/visible/mousefocus/textedited/mousemoved——实测真窗口首帧
-- 即达，且随鼠标位置/焦点态漂移）不记不分发：tee 确定性口径 = 不注入则
-- in 段为空。注入脚本 --inject-click <x>,<y>（帧 1）/ --inject-key <key>
--（帧 2）：帧首 push、poll 前——同帧取出（帧号 = 帧块号 0-based）。
local kernel = require("kernel")

local FIXED_DT = 0.016667
-- 拓扑 C 无头（B4 阶段三 Task 4）：FR_HEADLESS=1 + conf.lua t.window=false。
-- 捕获/tee 全 GL-free；验证帧回读与回放跳过（探针定论的仅剩 GL 依赖点）。
local HEADLESS = os.getenv("FR_HEADLESS") == "1"

-- ------------------------------------------------------------------
-- 参数解析（l0/main.lua 同款：arg[1]=游戏目录，游戏参数从 arg[2] 起）
-- ------------------------------------------------------------------
local args = {}
if type(arg) == "table" then
	local i = 2
	while arg[i] ~= nil do
		args[#args + 1] = arg[i]
		i = i + 1
	end
end

local function get_val(flag)
	for i, a in ipairs(args) do
		if a == flag then return args[i + 1] end
	end
	return nil
end

-- 拓扑 B 离线回放（B4 阶段三 Task 3）：--offline-replay <path> 独占模式——
-- 不跑 kernel/run loop（也不吃 --out/--sha），main.lua 载入时 graphics 模块
-- 已创建（boot.lua love.init 先 require("main")），直接调 C 入口：读流 →
-- 逐帧 capture_exec_row 执行 → Canvas 回读 CRC 打印 D_CRC，跑完即退。
local OFFLINE_PATH = get_val("--offline-replay")
if OFFLINE_PATH then
	local ok, rc = xpcall(love.graphics._frOfflineReplay, debug.traceback, OFFLINE_PATH)
	io.stdout:flush()
	if not ok then
		io.stderr:write("fr-demo offline error:\n", tostring(rc), "\n")
		io.stderr:flush()
		os.exit(1)
	end
	os.exit(rc)
	return
end

-- tee₁ 模式：--out 存在即启用；缺省 = 现行验证 rig（stdout 报告，不写文件）。
local OUT_PATH = get_val("--out")
local TEE = OUT_PATH ~= nil
if TEE and os.getenv("FR_CAPTURE") ~= "1" then
	io.stderr:write("l0-demo: --out tee 需 FR_CAPTURE=1（否则会话只有头行）\n")
end
local FRAMES = 4
local SHA, SEED = nil, 0

if TEE then
	local fv = get_val("--frames")
	if fv ~= nil then
		FRAMES = tonumber(fv)
		if not FRAMES or FRAMES < 1 or math.floor(FRAMES) ~= FRAMES then
			io.stderr:write("fr-demo: --frames 须为正整数\n")
			os.exit(2)
		end
	end
	SHA = get_val("--sha")
	if not SHA or #SHA ~= 64 or SHA:find("[^0-9a-f]") then
		io.stderr:write("fr-demo: tee 模式缺 --sha <64hex>（cmd.tbl 指纹，调用侧算好传入）\n")
		os.exit(2)
	end
	local sv = get_val("--seed")
	if sv ~= nil then
		SEED = tonumber(sv)
		if not SEED or SEED < 0 or math.floor(SEED) ~= SEED then
			io.stderr:write("fr-demo: --seed 须为非负整数\n")
			os.exit(2)
		end
	end
end

-- B5 Task 1 注入脚本：--inject-click <x>,<y>（帧 1 push mousepressed）、
-- --inject-key <key>（帧 2 push keypressed）。帧号 = 帧块号（0-based）：
-- 帧首 push、poll 前 → 同帧 poll 取出（见 frame_body）。
local INJECT_CLICK, INJECT_KEY = nil, nil
local ic = get_val("--inject-click")
if ic then
	local xs, ys = ic:match("^(%-?%d+),(%-?%d+)$")
	if not xs then
		io.stderr:write("fr-demo: --inject-click 须为 <x>,<y> 整数对\n")
		os.exit(2)
	end
	INJECT_CLICK = { x = tonumber(xs), y = tonumber(ys) }
end
INJECT_KEY = get_val("--inject-key")

-- C 层会话缓冲全会话累积不消费（fr_capture.h：take 是拷贝）；帧界由 Lua 侧
-- 按已取走行数切 delta——每帧的捕获行恰落在该帧 op 记录点之后（验证帧的
-- Canvas 捕获也归该帧）。
local taken = 0
local frame_rows = {}                        -- frame_rows[f] = 第 f 帧的 out 行
local frame_in_rows = {}                     -- frame_in_rows[f] = 第 f 帧的 ev 行（B5 Task 1）

local function take_delta()
	local all = love.graphics._frTakeSession()
	local delta = {}
	for i = taken + 1, #all do
		delta[#delta + 1] = all[i]
	end
	taken = #all
	return delta
end

-- ---------------- B5 Task 1：ev 行拼行（格式源对齐 l0/fr.lua） ----------------
-- 与 l0/fr.lua 的 esc/f/tok_variant 同构（单一格式源原则的 Lua 侧镜像；
-- C 出口形态留待后续批次统一）。

-- 字符串转义：\ " \n \r \t 转义；其余 C0 与 U+2028/U+2029 拒收
-- （canonical 文本行不容纳未转义控制字符，防行撕裂——fr-spec §6 封版）。
-- C0 检查逐字节做（LuaJIT 下源码 \0 转义进 pattern 不可靠）。
local function esc(s)
	for i = 1, #s do
		local b = s:byte(i)
		if b < 32 and b ~= 9 and b ~= 10 and b ~= 13 then
			error("fr-demo: 控制字符禁入 canonical 文本")
		end
	end
	if s:find("\226\128\168", 1, true) or s:find("\226\128\169", 1, true) then
		error("fr-demo: U+2028/U+2029 禁入 canonical 文本")
	end
	s = s:gsub("\\", "\\\\")
	s = s:gsub('"', '\\"')
	s = s:gsub("\n", "\\n")
	s = s:gsub("\r", "\\r")
	s = s:gsub("\t", "\\t")
	return s
end

-- 量化域文本：%.6f 定长，NaN/Inf 拒收
local function fnum(v)
	if v ~= v or v == math.huge or v == -math.huge then
		error("fr-demo: NaN/Inf 禁入数值域")
	end
	return string.format("%.6f", v)
end

-- variant → token：num <%.6f> / str "<引号串>" / bool 0|1（poll 返回值只会有
-- number/boolean/string——白名单事件不含 userdata 形态）
local function tok_variant(v)
	local tv = type(v)
	if tv == "boolean" then return v and "bool 1" or "bool 0" end
	if tv == "number" then return "num " .. fnum(v) end
	if tv == "string" then return 'str "' .. esc(v) .. '"' end
	error("fr-demo: 非法 variant: " .. tostring(v))
end

-- in 段白名单（cmd.tbl ev 行 type=handlers 键名的离散输入子集）：环境/连续
-- 事件（focus/visible/mousefocus/textedited/mousemoved）不记不分发——
-- mousemoved/focus 类随鼠标位置与焦点态漂移，进流则 tee 确定性破（实测
-- 真窗口首帧 poll 即达 visible/focus/textedited/mousefocus/mousemoved 五条）。
local IN_EVENTS = {
	keypressed = true, keyreleased = true, textinput = true,
	mousepressed = true, mousereleased = true, wheelmoved = true,
}

-- IR 适配层：kernel 说 IR 语义（rgba8 0-255、mode 0=fill 1=line，cmd.tbl 口径），
-- 翻译为 love.graphics 的 0-1 浮点 + 字符串 mode + setColor 状态折算。
local ir_graphics = {}

local function setircolor(r, g, b, a)
	love.graphics.setColor(r / 255, g / 255, b / 255, a / 255)
end

function ir_graphics.clear(r, g, b, a)
	love.graphics.clear(r / 255, g / 255, b / 255, a / 255)
end

-- 资产号 → 对象映射（kernel 说资产号，L0 main.lua 注册序口径：image=1、
-- font=2；quad=0 = 空资产哨兵）。捕获版值 = 代理 userdata（w_frNew* 返回），
-- 真版值 = 真 Image/Font——kernel 侧两种模式同构。
local FR_ASSETS = {}

-- 资产注册（B4 阶段三真字节口径）：newImage 传真 ImageData（32x32 全红——
-- 捕获版登记其 RGBA 字节 hex，物化回放回读同色，像素锚 (20,30)=红）；
-- newFont(14) 传真参数（捕获版登记内嵌 Vera.ttf 字节 + size）。
local function register_assets()
	local img_data = love.image.newImageData(32, 32)
	img_data:mapPixel(function() return 1, 0, 0, 1 end) -- 全红
	FR_ASSETS[1] = love.graphics.newImage(img_data)
	FR_ASSETS[2] = love.graphics.newFont(14)
end

function ir_graphics.draw(tex, quad, x, y, rot, sx, sy, ox, oy, r, g, b, a)
	setircolor(r, g, b, a)
	-- quad=0 = 整图哨兵（L0 口径）：demo 无真 Quad，直传整图形态。
	-- 捕获版第 2 参为 number → quad 记 0，行 `draw <tex> 0 x y ...` 同构。
	love.graphics.draw(FR_ASSETS[tex], x, y, rot, sx, sy, ox, oy)
end

function ir_graphics.print(font, text, x, y, rot, sx, sy, r, g, b, a)
	setircolor(r, g, b, a)
	love.graphics.print(text, FR_ASSETS[font], x, y, rot, sx, sy)
end

-- cmd.tbl: rect mode 0=fill 1=line（love DrawMode 枚举反向，C++ 侧换算）。
local IR_MODES = { [0] = "fill", [1] = "line" }

function ir_graphics.rectangle(mode, x, y, w, h, rx, ry, width, r, g, b, a)
	setircolor(r, g, b, a)
	love.graphics.setLineWidth(width)
	love.graphics.rectangle(IR_MODES[mode] or "fill", x, y, w, h, rx, ry)
end

function love.update(dt)
	kernel.update(FIXED_DT)
end

function love.draw()
	kernel.draw(ir_graphics)
end

local VERIFY_FRAME = 3
local frame = 0

-- 验证帧自证：三点像素回读（背景黑 / 方块区真红物化 / 红条红）+ 汇总行。
local function verify_frame()
	-- 真 Canvas 开关无关（语义替代版不建真 Canvas，见 fr_capture.h 边界）；
	-- _frNewCanvasReal 定 dpiScale=1（像素域=逻辑域）。
	local c = love.graphics._frNewCanvasReal(640, 480)
	love.graphics.setCanvas(c)
	love.draw()                 -- 捕获模式：记录行（真执行被抑制）；关闭模式：真画
	love.graphics._frReplay()   -- 捕获模式：行缓冲回放到 canvas；关闭模式：空缓冲 no-op
	love.graphics.setCanvas()   -- 先解绑：active Canvas 不许 newImageData（11.5 实测报错）
	local img = c:newImageData()
	local pts = {
		{ 400, 100, 0, 0, 0 },       -- 背景（黑 clear）
		{ 20, 30, 255, 0, 0 },       -- 方块区（hero 11,20 起 32x32；真字节物化=真红）
		{ 320, 468, 255, 0, 0 },     -- 红条（0,456,640,24 fill）
	}
	local npass = 0
	for _, p in ipairs(pts) do
		local r, g, b = img:getPixel(p[1], p[2])
		r, g, b = math.floor(r * 255 + 0.5), math.floor(g * 255 + 0.5), math.floor(b * 255 + 0.5)
		print(string.format("PIXEL %d %d %d %d %d", p[1], p[2], r, g, b))
		if r == p[3] and g == p[4] and b == p[5] then npass = npass + 1 end
	end
	print(string.format("PIXELS %d %d", #pts, npass))
	-- D_CRC（B4 阶段三 Task 3）：验证帧画布全字节 CRC32（_frImageDataCRC
	-- 与离线回放同一 C 采集函数）——拓扑 B 对拍锚：离线回放同一流文件的
	-- `D_CRC 3`（前 3 帧块执行后的画布）与本行同值。tee/非 tee 都打。
	print(string.format("D_CRC %d %d", frame, love.graphics._frImageDataCRC(img)))
	-- 会话行（测试判据）：资产行 + out 段行（clear/draw/print/rect）。
	-- tee 模式不在此打印/退出：验证帧捕获行归本帧 delta，会话末统一组装写
	-- 文件（Task 4：Canvas 回读与 tee 并存，先验证后 tee）。
	if not TEE then
		local assets = love.graphics._frTakeAssets()
		for _, row in ipairs(assets) do print(row) end
		local rows = love.graphics._frTakeSession()
		for _, row in ipairs(rows) do print(row) end
		io.stdout:flush()
		os.exit(0)
	end
end

-- tee₁ 会话写出（l0/main.lua 组装段同构）：头行 + cmnt 固定文本 + 帧块
--（帧号 0 起、与序数一致——fr_session.parse_text 连续性校验）。文件 UTF-8/LF
--（"wb" 防 CRT 文本模式 CRLF 改写），末尾单 LF（canonical）。
local function write_session()
	local parts = {
		("frb1 640 480 %s base %d"):format(SHA, SEED),
		"cmnt fork capture rig session",
	}
	local assets = love.graphics._frTakeAssets()
	for f = 1, frame do
		parts[#parts + 1] = ("frame %d"):format(f - 1)
		parts[#parts + 1] = ("dt %.6f"):format(FIXED_DT)
		if f == 1 then
			for _, row in ipairs(assets) do parts[#parts + 1] = row end
		end
		-- B5 Task 1：帧内行序 dt → asset（首帧）→ ev（in 段）→ out
		local ins = frame_in_rows[f]
		if ins then
			for _, row in ipairs(ins) do parts[#parts + 1] = row end
		end
		for _, row in ipairs(frame_rows[f]) do parts[#parts + 1] = row end
	end
	local fh = assert(io.open(OUT_PATH, "wb"))
	fh:write(table.concat(parts, "\n") .. "\n")
	fh:close()
end

-- 帧体（错误边界内）：rig 的一切行为。
local function frame_body()
	local in_rows = nil                -- 本帧 ev 行（B5 Task 1；nil = 无事件帧）
	if love.event then
		-- 注入脚本（帧首 push、poll 前——同帧取出）：参数序对齐 OS 事件形态
		--（Event.cpp convert 实测）：mousepressed = x y button istouch presses
		-- 五参；keypressed = key scancode isrepeat 三参（注入以 key 兼作 scancode）。
		if INJECT_CLICK and frame == 1 then
			love.event.push("mousepressed", INJECT_CLICK.x, INJECT_CLICK.y, 1, false, 1)
		end
		if INJECT_KEY and frame == 2 then
			love.event.push("keypressed", INJECT_KEY, INJECT_KEY, false)
		end

		love.event.pump()
		-- poll 逐条取全参（poll_i 返回 name+全部存储参数，无 5 参上限——
		-- generic for 的 a,b,c,d,e 绑定反而截尾，故 table 收全）。
		local pi = love.event.poll()
		local slot = 0
		while true do
			local ev = { pi() }
			local name = ev[1]
			if name == nil then break end
			if name == "quit" then
				return 0
			end
			if IN_EVENTS[name] then
				local argc = #ev - 1
				local toks = { ('ev %d "%s" 0 %d'):format(slot, esc(name), argc) }
				for i = 2, #ev do
					toks[#toks + 1] = tok_variant(ev[i])
				end
				slot = slot + 1
				in_rows = in_rows or {}
				in_rows[#in_rows + 1] = table.concat(toks, " ")
				-- 分发（poll 快照逐条、当帧完成）：有则调，无则只记录
				local h = kernel.handlers[name]
				if h then h(unpack(ev, 2)) end
			end
		end
	end

	if love.timer then love.timer.step() end
	love.update(FIXED_DT)

	frame = frame + 1
	local verified = false
	-- 无头（拓扑 C）跳过验证帧——Canvas 回读是 GL 依赖点（探针定论）；本帧
	-- 捕获行走常规 love.draw() 路径，与有窗版逐字节同源（tee 一致性锚）。
	if frame == VERIFY_FRAME and not HEADLESS then
		-- 精确一次（tee 模式循环延续到 FRAMES，>= 会在后续帧重复验证）；
		-- 非 tee：内部 exit(0)；tee：Canvas 验证后返回。
		verify_frame()
		verified = true
	end

	if not verified then
		if love.graphics then
			love.draw()
			love.graphics.present() -- 捕获模式：w_present 钩先回放后上屏
		end
	end

	if TEE then
		frame_rows[frame] = take_delta()
		if in_rows then frame_in_rows[frame] = in_rows end
		if frame >= FRAMES then
			write_session()
			io.stdout:flush()
			os.exit(0)
		end
	elseif HEADLESS and frame >= FRAMES then
		os.exit(0)             -- 无头非 tee：跑满即退（无验证帧出口）
	end

	if love.timer then love.timer.sleep(0.001) end
end

function love.run()
	register_assets()  -- 资产号自 1 起，须在任何 draw 前（kernel.draw 按号引用）
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