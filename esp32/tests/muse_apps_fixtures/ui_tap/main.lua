-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- Two buttons, one subscribed to "tap" and the other to "click", which is the same event.
local app = {}

function app.start()
    local go = ui.button{text = "Go", y = 60}
    go:on("tap", function(w, event) print("tapped", tostring(w), event) end)
    local alias = ui.button{text = "Alias", y = 120}
    alias:on("click", function(w, event) print("clicked", tostring(w), event) end)
    ui.label{text = "Plain"}
end

return app
