local frame = require("frame")
local keymap = frame.keymap
local ctx = frame.context

frame.setup({
  input = { preset = "default", prefix = "C-b" },
  terminal = { scrollback_lines = 100000 },
  ui = { status_line = true },
  launch = { default_program = { "/bin/sh", "-l" } },
})

ctx.set("resize", {
  label = "RESIZE",
  lifetime = "persistent",
  unbound = "consume",
})

keymap.set("normal", "M-d", "split_left_right")
keymap.set("normal", "M-r", ctx.push("resize"))
keymap.set("resize", "q", ctx.pop())
keymap.del("normal", "Cmd-c")
