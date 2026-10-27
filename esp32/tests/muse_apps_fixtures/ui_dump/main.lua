-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- Two widgets for the dump: a label that sizes to its content, and a button that doesn't.
local app = {}

function app.start()
    ui.label{text = "Time", x = 5, y = -10}
    ui.button{text = "OK", w = 120, h = 48, hidden = true}
end

return app
