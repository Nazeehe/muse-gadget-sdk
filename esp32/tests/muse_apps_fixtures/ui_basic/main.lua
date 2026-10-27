-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- A widget's life: made, changed, read and deleted. Each line it prints is checked by tests/test_muse_apps_ui.py.
local app = {}

function app.start()
    local w = ui.label{text = "Hello", x = 10, y = 20, font = "sans28", color = "#ff8800"}
    print(tostring(w), w:get("text"), w:get("x"), w:get("y"), w:get("font"), w:get("color"))
    w:set{text = "World", x = 11}
    print(w:get("text"), w:get("x"))
    print(w:get("w"), w:get("h"), w:get("align"), w:get("hidden"), w:get("opacity"))
    w:delete()
    print(pcall(w.get, w, "text"))
    print(tostring(w))
end

return app
