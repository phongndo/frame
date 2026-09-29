local frame = require("frame")

frame.extension.set("statusline", { frame.bundled_ui, "status" })
frame.command.register("session.manager", {
  description = "Find a session, tab, or pane",
  timeout_ms = 600000,
  argv = { frame.bundled_ui, "sessions" },
})
frame.keymap.set("prefix", "s", "session.manager", "base")
