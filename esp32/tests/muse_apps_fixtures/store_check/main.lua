-- Copyright (c) Meta Platforms, Inc. and affiliates.
-- SPDX-License-Identifier: Apache-2.0

-- The board check of the store and the sys module. start() counts its own runs in the store and prints the number, so that
-- `>app.stop` then `>app.launch` shows the value that was saved at the stop read back. The message "report" says what the
-- device reports of itself: the battery, whether it is on Wi-Fi, and how long the SSID is (never the SSID: a log must not carry it).
local app = {}

function app.start()
    local runs = store.get("runs", 0) + 1
    store.set("runs", runs)
    print("store_check runs " .. runs)
end

function app.message(event)
    local battery, wifi = sys.battery(), sys.wifi()
    return {runs = store.get("runs", 0), battery = battery, connected = wifi.connected, ssid_len = #wifi.ssid}
end

return app
