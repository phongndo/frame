local frame = require("frame")

frame.command.register("nav.pick", {
  description = "Choose a Session, Tab, or Pane",
  timeout_ms = 120000,
  argv = { "python3", os.getenv("HOME") .. "/.config/frame/picker.py" },
})
frame.keymap.set("prefix", "p", "nav.pick")
