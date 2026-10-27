-- demo/conf.lua —— fork demo 配置（B4-2 Task 0）：窗口开启（上屏验收需要真窗口，
-- 与 l0 的 headless t.window=false 相对）；尺寸对齐 kernel.draw 的 640x480 坐标域。
-- B4 阶段三 Task 4：FR_HEADLESS=1 → 拓扑 C 无头（t.window=false，l0 同款口径）——
-- 捕获/tee 路径全 GL-free（探针实证：行记录/资产登记/会话缓冲不碰 GL），
-- 仅跳过 GL 依赖的回放与 Canvas 回读（demo/main.lua 同分支）。
function love.conf(t)
	if os.getenv("FR_HEADLESS") == "1" then
		t.window = false
	else
		t.window.title = "FR capture demo"
		t.window.width = 640
		t.window.height = 480
	end
	t.identity = "fr-fork-demo"
end
