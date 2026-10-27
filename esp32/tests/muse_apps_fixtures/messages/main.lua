-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

local app = {}
local state = "idle"

function app.message(event, data)
    if event == "status" then
        return {state = state}
    elseif event == "bad" then
        error("unknown event")
    end
end

return app
