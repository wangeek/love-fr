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
-- 严格档注入回放（B5 Task 2）：--sim <in.fr> 独占——不跑 love.run，独立
--                  帧循环重演内核（ev 行还原 push → poll 分发 → update 吃
--                  录制 dt → draw 捕获再生行，与录制 out 行逐帧比对）。
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
-- dt → 资产（仅 frame 0，checkpoint 注入口径）→ in 事件行 → out 捕获行
-- → end 段 s_crc）。参数：--frames N（缺省 4）、--sha <64hex>
--（cmd.tbl 指纹，调用侧算好传入；缺/坏报错 exit 2，L0 同款）、--seed n
--（缺省 0）。确定性：FIXED_DT 固定 + cmnt 固定 + 无随机 → 两次运行逐字节
-- 一致（测试锚）。验证帧（frame 3）的 Canvas 回读与 tee 并存：先验证后 tee。
-- B6 Task 1/1b（end 段 s_crc）：present 时刻 C 侧确定性 CRC——口径 =
-- 注册状态根（kernel.state，载入期 _frStateRoot 登记）可达子图（Task 1b
-- 裁定，D8 镜像范围；未注册根回退全堆）；验证帧与 sim 循环显式调
-- _frStateSubCRC，两路径同一 C 遍历；帧块尾 `s_crc <u32十进制>`。
-- B7a（end 段 d_crc）：s_crc 后紧跟 `d_crc <u32十进制>`——当帧 out 行
-- 文本（take_delta 同批行，口径 = clear/draw/print/rect，不含 dt/asset/
-- ev/st/err/s_crc）join "\n" 的 CRC32（cmd.tbl d_crc：out 记录 CRC32）。
-- 收集点 = tee 分支 take_delta 处（demo 拼串、C 侧 _frCRC32 算——算法
-- 单一真源在 C）；sim 循环同收集并纳入 SIM_CRC 双比对。
-- B6 Task 2（log 段 st 物理行，FR_LOGLEVEL=full）：present 钩/显式收集点
-- 影子镜像 diff 产 st/err 行（同帧纪律与 s_crc 一致）；验证帧与 sim 循环
-- 显式调 _frStateDiffRows；帧内行序 dt → asset（首帧）→ ev → st/err →
-- out → s_crc；头行 loglvl 随档（base|full）。sim 的 log 行纳入比对集。
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

-- B6 Task 1b：登记状态根——s_crc 口径 = kernel.state 可达子图 CRC。录制/
-- --sim 两进程的 args 表与解析簿记表形态天然不同（Task 1 实证，全堆口径
-- 曾 xfail），子图由同一代码+同一事件序列构建，跨进程等值可达。载入期
-- 登记，先于一切收集点（帧 1 present 钩 / 验证帧 / --sim 循环共用之）。
love.graphics._frStateRoot(kernel.state)

-- B6 确定性前提：关 JIT 编译（解释执行）。JIT 起编时机随热点计数漂移
--（验证帧的回读循环会把阈值提前推过线——窗口/无头两侧不同帧起编，
-- trace 元数据表进堆让 s_crc 跨模式不可比，实测定位）。捕获 rig 以
-- 确定性优先，4 帧跑分无关性能。
if jit then jit.off() end

local FIXED_DT = 0.016667
-- 拓扑 C 无头（B4 阶段三 Task 4）：FR_HEADLESS=1 + conf.lua t.window=false。
-- 捕获/tee 全 GL-free；验证帧回读与回放跳过（探针定论的仅剩 GL 依赖点）。
local HEADLESS = os.getenv("FR_HEADLESS") == "1"

-- B6 Task 2（diff 档）：FR_LOGLEVEL=full → present 钩/显式收集点同产 st 物理行
--（影子镜像 diff，C 侧 fr_statediff）进当帧 log 段；帧内行序 dt → asset →
-- ev → st/err → out → s_crc。头行 loglvl 随档（fr_session 三档域 off|base|full）。
local LOGLVL_FULL = os.getenv("FR_LOGLEVEL") == "full"

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

-- B5 Task 2：--sim <path> 独占模式（严格档注入回放——内核重演）。参数在此
-- 解析、实现在 write_session 之后的 sim 段（依赖 ir_graphics/register_assets/
-- take_delta/IN_EVENTS 先行定义；不与 --out/--inject 组合——独占帧循环，
-- 不跑 love.run/verify_frame）。
local SIM_PATH = get_val("--sim")

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
local frame_dcrcs = {}                       -- frame_dcrcs[f] = 第 f 帧的 d_crc（B7a）

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
	kernel.draw(ir_graphics, FR_ASSETS)
end

local VERIFY_FRAME = 3
local frame = 0

-- 验证帧自证：三点像素回读 + 汇总 + D_CRC。回读环整体在 C 侧
--（_frCanvasProbe：PIXEL/PIXELS/D_CRC 行 stdout 与旧 Lua 环逐字节同构；
-- 三采样点/期望色是 rig 常量，落 C——B6 实测：Lua 侧 getPixel/format/
-- print 环在验证帧留 live 残留表，污染末帧 s_crc 的窗口/无头一致性，
-- Canvas/setCanvas/draw/_frReplay 路径实测无残留）。
local function verify_frame()
	-- 真 Canvas 开关无关（语义替代版不建真 Canvas，见 fr_capture.h 边界）；
	-- _frNewCanvasReal 定 dpiScale=1（像素域=逻辑域）。
	local c = love.graphics._frNewCanvasReal(640, 480)
	love.graphics.setCanvas(c)
	love.draw()                 -- 捕获模式：记录行（真执行被抑制）；关闭模式：真画
	love.graphics._frReplay()   -- 捕获模式：行缓冲回放到 canvas；关闭模式：空缓冲 no-op
	love.graphics.setCanvas()   -- 先解绑：active Canvas 不许 newImageData（11.5 实测报错）
	love.graphics._frCanvasProbe(c, frame)
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
		("frb1 640 480 %s %s %d"):format(SHA, LOGLVL_FULL and "full" or "base", SEED),
		"cmnt fork capture rig session",
	}
	local assets = love.graphics._frTakeAssets()
	local scrcs = love.graphics._frTakeSCrcs()   -- B6：每帧堆 CRC（end 段）
	local sdiffs = LOGLVL_FULL and love.graphics._frTakeSDiffRows() or nil
	                                             -- B6 Task 2：每帧 log 段行
	for f = 1, frame do
		parts[#parts + 1] = ("frame %d"):format(f - 1)
		parts[#parts + 1] = ("dt %.6f"):format(FIXED_DT)
		if f == 1 then
			for _, row in ipairs(assets) do parts[#parts + 1] = row end
		end
		-- B5 Task 1：帧内行序 dt → asset（首帧）→ ev（in 段）→ st/err（log
		-- 段，full 级）→ out → s_crc（end 段）
		local ins = frame_in_rows[f]
		if ins then
			for _, row in ipairs(ins) do parts[#parts + 1] = row end
		end
		-- B6 Task 2：log 段 st 物理行 + err 行（diff 档；收集点 = present 钩/
		-- 验证帧显式，与 s_crc 同帧纪律——每帧恰一份）
		if sdiffs then
			assert(sdiffs[f] ~= nil, "fr-demo: 帧 " .. tostring(f) .. " 缺 diff 行（收集钩缺席？）")
			for _, row in ipairs(sdiffs[f]) do parts[#parts + 1] = row end
		end
		for _, row in ipairs(frame_rows[f]) do parts[#parts + 1] = row end
		-- B6 Task 1：end 段 s_crc（out 行后；cmd.tbl end 段 crc u32，十进制
		-- canonical）+ B7a d_crc（s_crc 后，帧块尾——当帧 out 行文本 CRC32，
		-- 收集点见 tee 分支）。
		assert(scrcs[f] ~= nil, "fr-demo: 帧 " .. tostring(f) .. " 缺 s_crc（收集钩缺席？）")
		parts[#parts + 1] = ("s_crc %d"):format(scrcs[f])
		assert(frame_dcrcs[f] ~= nil, "fr-demo: 帧 " .. tostring(f) .. " 缺 d_crc（收集点缺席？）")
		parts[#parts + 1] = ("d_crc %d"):format(frame_dcrcs[f])
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
		-- poll 逐条收 6 局部（name + 5 参：白名单事件最大 5 参——
		-- mousepressed x y button istouch presses）。不建 { pi() } 表：
		-- 环境事件（focus/mousemoved…）窗口版每帧多条、无头版为零，
		-- 表垃圾进 GC 链曾在全堆口径下让 s_crc 随窗口/无头模式漂移
		-- （B6 实测；Task 1b 子图口径已不依赖此约束，保留既定形态）；
		-- 参数列中无 nil（love 事件载荷不含 nil），尾部
		-- nil 计数即 argc。
		local pi = love.event.poll()
		local slot = 0
		while true do
			local name, e1, e2, e3, e4, e5 = pi()
			if name == nil then break end
			if name == "quit" then
				return 0
			end
			if IN_EVENTS[name] then
				local argc = e5 ~= nil and 5 or e4 ~= nil and 4 or e3 ~= nil and 3
					or e2 ~= nil and 2 or e1 ~= nil and 1 or 0
				local toks = { ('ev %d "%s" 0 %d'):format(slot, esc(name), argc) }
				if argc >= 1 then toks[#toks + 1] = tok_variant(e1) end
				if argc >= 2 then toks[#toks + 1] = tok_variant(e2) end
				if argc >= 3 then toks[#toks + 1] = tok_variant(e3) end
				if argc >= 4 then toks[#toks + 1] = tok_variant(e4) end
				if argc >= 5 then toks[#toks + 1] = tok_variant(e5) end
				slot = slot + 1
				in_rows = in_rows or {}
				in_rows[#in_rows + 1] = table.concat(toks, " ")
				-- 分发（poll 快照逐条、当帧完成）：有则调，无则只记录
				--（尾部 nil 多传无害——handler 只取前缀参）
				local h = kernel.handlers[name]
				if h then h(e1, e2, e3, e4, e5) end
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
		-- B6 状态 CRC（Task 1b 子图口径）：验证帧不走 present（w_present
		-- 钩不触发），显式等价收集——与 present 钩同一 C 遍历（根已载入期
		-- 登记）；collectgarbage 沿用 present 路径既定口径（子图外噪声本
		-- 就不入 CRC）。无头本帧走 present 钩，两侧每帧恰一份。
		collectgarbage("collect")
		love.graphics._frStateSubCRC()
		-- B6 Task 2：diff 档同点显式收集（full 级）——与 present 钩同一 C
		-- 遍历，行入当帧缓存（write_session 组装 log 段）。
		if LOGLVL_FULL then love.graphics._frStateDiffRows() end
		-- 精确一次（tee 模式循环延续到 FRAMES，>= 会在后续帧重复验证）；
		-- 非 tee：内部 exit(0)；tee：Canvas 验证后返回。
		verify_frame()
		verified = true
	end

	if not verified then
		if love.graphics then
			love.draw()
			-- B6：present 前清垃圾——s_crc 是 live 堆快照：未清扫的死表
			-- 会随 OS 事件 interning 量抖动清扫时机（实测双跑偶发漂移），
			-- 收集前 collectgarbage 把噪声挡在快照外（公开 API，非 statediff
			-- 层行为——C 遍历保持只读红线）。
			collectgarbage("collect")
			love.graphics.present() -- 捕获模式：w_present 钩先回放后上屏
		end
	end

	if TEE then
		frame_rows[frame] = take_delta()
		-- B7a d_crc：当帧 out 行（take_delta 同批）文本 join "\n" 的 CRC32
		--（cmd.tbl d_crc：out 记录 CRC32）。行集口径 = out 段 op
		--（clear/draw/print/rect，不含 dt/asset/ev/st/err/s_crc）；demo 拼串、
		-- C 侧 _frCRC32 算（算法单一真源在 C）。空帧 join 为 "" → CRC=0，
		-- 如实记录（demo 每帧恒有 out 行，不达）。
		frame_dcrcs[frame] = love.graphics._frCRC32(table.concat(frame_rows[frame], "\n"))
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

-- ---------------- B5 Task 2：--sim 严格档注入回放（内核重演） ----------------
-- 独立帧循环（不跑 love.run/verify_frame）：读 .fr 文本流 → 逐帧把 ev 行
-- 还原为 Lua 值 push 进事件队列（与录制同链路：帧首 push → pump → poll
-- 取出 → 分发 kernel.handlers）→ kernel.update（dt 取流行，FIXED_DT 不用
-- ——重演吃录制的 dt）→ kernel.draw(ir_graphics, FR_ASSETS)（FR_CAPTURE=1 捕获再生
-- out 行）→ 与录制 out 行逐行比对。纯行比对无 Canvas 回读——GL-free，
-- FR_HEADLESS=1 可同用。stdout 逐帧 `SIM <frame> MATCH|MISMATCH <ndiff>
-- | REC: <首差异录制行> | GEN: <首差异再生行>`（缺侧记 "-"；ev 条数漂移
-- 计入差异）；末行 `SIM SUMMARY <frames> <match> <mismatch> <rc>`，全
-- MATCH rc=0 否则 rc=1。.frb 本批不吃（fr-as 可转文本形态，如实登记）。

-- 反转义（esc 的逆）：\\ \" \n \r \t
local function unesc(s)
	local out, i = {}, 1
	while i <= #s do
		local c = s:sub(i, i)
		if c == "\\" then
			local n = s:sub(i + 1, i + 1)
			if n == "n" then out[#out + 1] = "\n"
			elseif n == "r" then out[#out + 1] = "\r"
			elseif n == "t" then out[#out + 1] = "\t"
			elseif n == "\\" or n == '"' then out[#out + 1] = n
			else error("fr-demo: 非法转义 \\" .. n) end
			i = i + 2
		else
			out[#out + 1] = c
			i = i + 1
		end
	end
	return table.concat(out)
end

-- ev 行解析：`ev <slot> "<name>" <src> <argc> <variant...>` → {name=, args=}
--（variant 还原：num→tonumber、str→去引号反转义、bool→"1"==v——tok_variant 逆）。
local function parse_ev_row(line)
	local pos = 1
	-- 取一 token：以 " 起则按引号串取（转义原样保留，调用方 unesc）；否则到空白止
	local function next_tok()
		while pos <= #line and line:sub(pos, pos) == " " do pos = pos + 1 end
		if pos > #line then return nil end
		if line:sub(pos, pos) == '"' then
			local i, out = pos + 1, {}
			while true do
				local ch = line:sub(i, i)
				if ch == "" then error("fr-demo: ev 行引号串未闭合") end
				if ch == '"' then pos = i + 1 return table.concat(out) end
				if ch == "\\" then
					out[#out + 1] = ch .. line:sub(i + 1, i + 1)
					i = i + 2
				else
					out[#out + 1] = ch
					i = i + 1
				end
			end
		end
		local j = line:find(" ", pos, true) or (#line + 1)
		local t = line:sub(pos, j - 1)
		pos = j
		return t
	end
	local tag = next_tok()
	if tag ~= "ev" then error("fr-demo: 非 ev 行进 parse_ev_row：" .. line) end
	next_tok()                              -- slot（单事件帧恒 0，解析即弃）
	local name = unesc(next_tok())          -- 事件名带引号去除
	next_tok()                              -- src（严格档只注入，值不消费）
	local argc = tonumber(next_tok())
	if not argc or argc < 0 then error("fr-demo: ev 行 argc 非法：" .. line) end
	local args = {}
	for _ = 1, argc do
		local kind = next_tok()
		local val = next_tok()
		if kind == "num" then
			local n = tonumber(val)
			if not n then error("fr-demo: num variant 非法：" .. line) end
			args[#args + 1] = n
		elseif kind == "bool" then
			args[#args + 1] = (val == "1")
		elseif kind == "str" then
			args[#args + 1] = unesc(val)
		else
			error("fr-demo: 未知 variant: " .. tostring(kind))
		end
	end
	return { name = name, args = args }
end

-- 流解析：.fr 文本 → 帧表 {{dt=num, ev={{name,args}}, out={行}, scrc=num,
-- dcrc=num}}。分类按本 rig 会话组装口径：首帧块前只许 frb1/cmnt；帧块内
-- dt/asset/ev/s_crc/d_crc 归类（asset 行跳过——sim 侧 register_assets 同序
-- 重建，不比对；s_crc=B6 end 段堆 CRC、d_crc=B7a end 段 out 行 CRC，
-- 收集后与重演侧逐帧比对），其余整行进 out（与再生行逐字节比对）。
local function parse_stream(path)
	local fh, e = io.open(path, "rb")
	if not fh then
		io.stderr:write("fr-demo: 流文件打不开：", tostring(e), "\n")
		os.exit(2)
	end
	local data = fh:read("*a")
	fh:close()
	local frames, cur, saw_head = {}, nil, false
	for line in data:gmatch("[^\r\n]+") do
		local op = line:match("^(%S+)")
		if op == "frame" then
			local n = tonumber(line:match("^frame (%d+)$"))
			if not n or n ~= #frames then
				io.stderr:write("fr-demo: frame 序号不连续：", line, "\n")
				os.exit(2)
			end
			cur = { dt = nil, ev = {}, out = {}, logrows = {} }
			frames[#frames + 1] = cur
		elseif not cur then
			if op == "frb1" then
				saw_head = true
			elseif op ~= "cmnt" then
				io.stderr:write("fr-demo: 首帧块前的未知行：", line, "\n")
				os.exit(2)
			end
		elseif op == "dt" then
			local v = tonumber(line:match("^dt (%S+)$"))
			if not v then
				io.stderr:write("fr-demo: dt 行非法：", line, "\n")
				os.exit(2)
			end
			cur.dt = v
		elseif op == "asset" then
			-- 资产行：sim 侧 register_assets 重建（顺序/内容与录制同源）
		elseif op == "s_crc" then
			-- B6 end 段堆 CRC：收集进帧表（录制侧值），旧流无此行则留 nil
			local v = line:match("^s_crc (%d+)$")
			if not v then
				io.stderr:write("fr-demo: s_crc 行非法：", line, "\n")
				os.exit(2)
			end
			cur.scrc = tonumber(v)
		elseif op == "d_crc" then
			-- B7a end 段 out 行 CRC：收集进帧表（录制侧值），旧流无此行则留 nil
			local v = line:match("^d_crc (%d+)$")
			if not v then
				io.stderr:write("fr-demo: d_crc 行非法：", line, "\n")
				os.exit(2)
			end
			cur.dcrc = tonumber(v)
		elseif op == "st" or op == "err" then
			-- B6 Task 2：log 段行（full 级 diff 档产物）——重演比对集（含
			-- err：不受支持的 table 引用槽/表重建行与 st 同源同确定性）
			cur.logrows[#cur.logrows + 1] = line
		elseif op == "ev" then
			cur.ev[#cur.ev + 1] = parse_ev_row(line)
		else
			cur.out[#cur.out + 1] = line
		end
	end
	if not saw_head or #frames == 0 then
		io.stderr:write("fr-demo: 流缺 frb1 头行或帧块\n")
		os.exit(2)
	end
	for i, fr in ipairs(frames) do
		if not fr.dt then
			io.stderr:write("fr-demo: 帧 ", tostring(i - 1), " 缺 dt 行\n")
			os.exit(2)
		end
	end
	return frames
end

if SIM_PATH then
	-- 错误边界（offline 分支同款）：载入期裸错会落 love 蓝屏（交互挂住），
	-- sim 一切错误收口为 stderr traceback + exit(1)。
	local ok, rc = xpcall(function()
		if os.getenv("FR_CAPTURE") ~= "1" then
			io.stderr:write("fr-demo: --sim 需 FR_CAPTURE=1（重演要捕获再生 out 行）\n")
			return 2
		end
		if SIM_PATH:match("%.frb$") then
			io.stderr:write("fr-demo: --sim 本批只吃 .fr 文本（.frb 先经 fr-as 转）\n")
			return 2
		end
		local frames = parse_stream(SIM_PATH)
		register_assets()
		local nmatch, nmis = 0, 0
		-- B7a：SIM_CRC 双比对（s=堆子图 CRC、d=当帧 out 行 CRC）的判定函数
		--——旧流无对应行则 skip，不作判定。
		local function crc_st(rec, genv)
			if rec == nil then return "skip" end
			return rec == genv and "ok" or "mis"
		end
		for fi, fr in ipairs(frames) do
			-- ① ev 行还原 push（帧首，同录制注入位）
			for _, ev in ipairs(fr.ev) do
				love.event.push(ev.name, unpack(ev.args))
			end
			-- ② 同录制链路：pump → poll → 白名单内分发（环境/连续事件不分发）
			local dispatched = 0
			love.event.pump()
			local pi = love.event.poll()
			while true do
				local ev = { pi() }
				local name = ev[1]
				if name == nil then break end
				if IN_EVENTS[name] then
					dispatched = dispatched + 1
					local h = kernel.handlers[name]
					if h then h(unpack(ev, 2)) end
				end
			end
			-- ③ update 吃录制 dt → draw 捕获再生行
			kernel.update(fr.dt)
			kernel.draw(ir_graphics, FR_ASSETS)
			-- B6 状态 CRC（Task 1b 子图口径）：present 等价点收集，与录制侧
			-- 同一出口（_frStateSubCRC，根已载入期登记）——子图只含
			-- kernel.state，本进程 parse/簿记表不进 CRC，跨进程等值可达；
			-- collectgarbage 沿用录制侧既定口径。
			collectgarbage("collect")
			local scrc = love.graphics._frStateSubCRC()
			-- B6 Task 2：diff 档同点收集（FR_LOGLEVEL=full 时非空）——st/err
			-- 行纳入 SIM 比对集（状态演化重演一致的强判据）；档位不对称
			--（流有 log 行而本侧 base 或反之）→ 空集对非空集，如实 MISMATCH。
			local genlog = LOGLVL_FULL and love.graphics._frStateDiffRows() or {}
			local gen = take_delta()
			-- B7a：重演侧同收集 d_crc——当帧再生 out 行（take_delta 同批）
			-- 文本 join "\n" 的 CRC32，与录制侧同口径（demo 拼串、C 侧
			-- _frCRC32 算）。
			local dcrc = love.graphics._frCRC32(table.concat(gen, "\n"))
			-- ④ 比对：ev 条数漂移（push/poll 往返丢失或 OS 噪声混入）+ 逐行位置差
			local ndiff, first_rec, first_gen = 0, nil, nil
			local function mark_diff(rec, genl)
				ndiff = ndiff + 1
				if not first_rec then first_rec, first_gen = rec, genl end
			end
			if dispatched ~= #fr.ev then
				mark_diff(("ev x%d"):format(#fr.ev), ("ev x%d"):format(dispatched))
			end
			for i = 1, math.max(#fr.out, #gen) do
				if fr.out[i] ~= gen[i] then
					mark_diff(fr.out[i] or "-", gen[i] or "-")
				end
			end
			for i = 1, math.max(#fr.logrows, #genlog) do
				if fr.logrows[i] ~= genlog[i] then
					mark_diff(fr.logrows[i] or "-", genlog[i] or "-")
				end
			end
			if ndiff == 0 then
				nmatch = nmatch + 1
				print(("SIM %d MATCH"):format(fi - 1))
			else
				nmis = nmis + 1
				print(("SIM %d MISMATCH %d | REC: %s | GEN: %s")
					:format(fi - 1, ndiff, first_rec, first_gen))
			end
			-- B6/B7a：堆 CRC + out 行 CRC 重演比对（独立于 out 行逐行比对——
			-- 内核状态重演全对的自证 + 流内 end 段行自洽的防篡改锚；旧流无
			-- 对应行则该维 skip，不作判定）。
			print(("SIM_CRC %d s=%s d=%s"):format(fi - 1,
				crc_st(fr.scrc, scrc), crc_st(fr.dcrc, dcrc)))
		end
		local rc2 = nmis == 0 and 0 or 1
		print(("SIM SUMMARY %d %d %d %d"):format(#frames, nmatch, nmis, rc2))
		io.stdout:flush()
		return rc2
	end, debug.traceback)
	if not ok then
		io.stderr:write("fr-demo sim error:\n", tostring(rc), "\n")
		io.stderr:flush()
		os.exit(1)
	end
	os.exit(rc)
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