-- demo/conf.lua —— fork demo 配置（B4-2 Task 0）：窗口开启（上屏验收需要真窗口，
-- 与 l0 的 headless t.window=false 相对）；尺寸对齐 kernel.draw 的 640x480 坐标域
function love.conf(t)
    t.window.title = "FR capture demo"
    t.window.width = 640
    t.window.height = 480
    t.identity = "fr-fork-demo"
end
