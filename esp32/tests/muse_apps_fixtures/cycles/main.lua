-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- Launch, run and stop leave nothing behind: print, require, json, tables, strings, a coroutine.
local util = require("util")
local helper = require("lib/helper")
print("top level", util.answer, helper.name)

local app = {}
local items = {}

function app.start()
    for i = 1, 50 do
        items[i] = {id = i, label = "item " .. i}
    end
    local co = coroutine.wrap(function() coroutine.yield(json.encode({n = #items})) end)
    print(co(), #items)
end

function app.message(event, data)
    return {event = event, data = data, count = #items}
end

function app.stop()
    print("stopping")
end

return app
