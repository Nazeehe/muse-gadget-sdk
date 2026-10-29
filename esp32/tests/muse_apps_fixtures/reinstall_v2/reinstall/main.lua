-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- Version 2 of the app that apphost's `install` replaces while it runs (tests/test_muse_apps_rt.py): another label, and what version 1
-- saved, read back in start().
local app = {}

function app.start()
    ui.label{text = "v2", x = 120, y = 260}
    print("v2 start " .. store.get("n", 0))
end

function app.stop()
    print("v2 stop")
end

return app
