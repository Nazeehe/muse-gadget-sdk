-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- util.lua is one of the app's files; main.lua is the way in.
local util = require("util")

local app = {}

function app.message(event, data)
    if event == "answer" then
        return {answer = util.answer}
    elseif event == "load" then
        local ok, value = pcall(require, data.name)
        if ok then
            return {ok = true, kind = type(value)}
        end
        return {ok = false, error = value}
    elseif event == "same" then
        return {same = require("util") == require("util.lua")}
    elseif event == "direct" then
        return {value = require(data.name)}
    end
end

return app
