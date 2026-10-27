-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- A handle is one object for as long as the app holds it, so a widget can be the key of a table: here the button
-- is found again inside its own tap handler, and a box is the same object whoever asks for the parent.
local app = {}
local names = {}

function app.start()
    local go = ui.button{text = "Go"}
    names[go] = "go"
    go:on("tap", function(w, event) print(names[w], rawequal(w, go), w == go, event) end)
    local box = ui.box{}
    local child = ui.label{parent = box}
    local twin = ui.label{parent = box}
    print(rawequal(child:get("parent"), box), rawequal(child:get("parent"), twin:get("parent")))
end

return app
