-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- Version 1 of the app that apphost's `install` replaces while it runs (tests/test_muse_apps_rt.py): a label, and a value it keeps
-- in the store, set by a message and not saved yet when the install comes, so that the replacement's flush is what saves it.
local app = {}

function app.start()
    ui.label{text = "v1", x = 120, y = 200}
    print("v1 start " .. store.get("n", 0))
end

function app.stop()
    print("v1 stop")
end

function app.message(event, data)
    if event == "set" then
        store.set("n", data.n)
        return true
    end
end

return app
