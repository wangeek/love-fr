-- demo/main.lua —— fork demo 入口骨架（B4-2 Task 0；改造自 l0/main.lua）
--
-- arg 表语义：fork 的 boot 与官方 11.5 一致，窗口模式沿用 arg[2] 起为游戏参数
-- （l0 headless 探针结论移植；窗口模式待 Task 2/4 接入 run loop 时重新探针确认）。
-- 本任务只放骨架：模式分发 + 窗口提示；capture run loop / 语义替代四件套 /
-- tee₁ 会话组装在 Task 2-4 逐步接入（--capture 模式暂为占位）。
local args = {}
if type(arg) == "table" then
    local i = 2
    while arg[i] ~= nil do
        args[#args + 1] = arg[i]
        i = i + 1
    end
end

local function has(flag)
    for _, a in ipairs(args) do
        if a == flag then return true end
    end
    return false
end

-- 模式表：
--   hint    无参数默认——窗口画用法提示（Task 0 骨架态）
--   capture Task 2-4 接入：捕获钩记录 love.graphics.* → present 前回放 → tee₁ 写 .fr
local mode = has("--capture") and "capture" or "hint"

if mode == "capture" then
    io.stderr:write("demo: --capture 骨架占位——捕获钩与 run loop 在 Task 2-4 接入\n")
end

-- 窗口提示文本用 ASCII（love 内置 Vera Sans 无 CJK 字形）
local hints = {
    "FR capture demo (B4-2 Task 0 skeleton)",
    "",
    "usage: love.exe demo [--capture]",
    "  (no args)  hint mode (this screen)",
    "  --capture  capture/replay pipeline - Task 2-4, not wired yet",
}

function love.draw()
    love.graphics.clear(0, 0, 0)
    love.graphics.print(table.concat(hints, "\n"), 16, 16)
end
