local frame = require("frame")

frame.command.register("work.shell", {
  description = "Open a shell tab",
  timeout_ms = 30000,
  handler = function(ctx, args)
    local result = ctx:proc({
      commands = {
        {
          command = "tab.new",
          session = { id = ctx.session },
          title = args[1] or "shell",
          argv = { "/bin/sh", "-l" },
        },
      },
    })
    assert(result.ok, "Could not open shell tab")
  end,
})
