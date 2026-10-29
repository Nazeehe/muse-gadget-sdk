-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- The example to start from: a ring, big digits, a greeting and a button.
-- Edit it, and `tools/muse/app.py watch PORT components/muse/apps/examples/hello` puts it on the gadget again.
local app = {}

function app.start()
    ui.bg("#000000")
    ui.arc{align = "center", w = 400, h = 400, min = 0, max = 100, value = 60, color = "#3aa8ff"}
    ui.label{text = "12:34", font = "digits96", align = "center", y = -50}
    local greeting = ui.label{text = "Hello from Muse", font = "sans20", align = "center", y = 30}
    ui.button{text = "Tap me", font = "sans20", align = "center", y = 110, w = 160, h = 56}:on("tap", function()
        print("tapped")
        greeting:set{text = "Tapped"}
    end)
end

return app
