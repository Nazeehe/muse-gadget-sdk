-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

local app = {}

function app.start()
    for i = 1, 2000 do
        print("line " .. i)
    end
end

return app
