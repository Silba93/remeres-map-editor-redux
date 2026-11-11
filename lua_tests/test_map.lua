-- @Title: Test Map API
-- @Description: Verification tests for this API category.
local framework = require("framework")

framework.test("map properties", function()
    if not app.hasMap() then
        print("Skipping map tests (no map open)")
        return
    end
    
    local map = app.map
    framework.assert(type(map.name) == "string", "map.name should be string")
    framework.assert(type(map.width) == "number", "map.width should be number")
    framework.assert(type(map.height) == "number", "map.height should be number")
    framework.assert(type(map.tileCount) == "number", "map.tileCount should be number")
end)

framework.test("map methods", function()
    if not app.hasMap() then return end
    
    local map = app.map
    framework.assert(type(map.getTile) == "function", "getTile should be function")
    framework.assert(type(map.getOrCreateTile) == "function", "getOrCreateTile should be function")
    framework.assert(type(map.tiles) ~= "nil", "map.tiles should exist")
end)

framework.test("getTile basic", function()
    if not app.hasMap() then return end
    
    local map = app.map
    local tile = map:getTile(100, 100, 7)
    -- tile can be nil if not exists, so check type
    framework.assert(type(tile) == "table" or type(tile) == "userdata" or type(tile) == "nil", "getTile should return tile or nil")
end)

framework.test("BlackTek zone transaction rollback preserves unknown fields", function()
    if not app.hasMap() then return end
    local map = app.map
    local name = "__rme_blacktek_transaction_test__"
    if map:getBlacktekZone(name) ~= nil then return end
    local enabled_before = map.blacktekZonesEnabled
    local ok = pcall(function()
        app.transaction("BlackTek rollback test", function()
            map:setBlacktekZone(name, [[
positions = ["100:100:7"]
flags = ["no_party", "future_flag"]
future_setting = { retained = true }
]])
            local text = map:getBlacktekZone(name)
            framework.assert(text:find("future_setting", 1, true) ~= nil, "Unknown settings must survive")
            framework.assert(text:find("future_flag", 1, true) ~= nil, "Unknown flags must survive")
            error("intentional rollback")
        end)
    end)
    framework.assert(not ok, "Transaction must propagate script errors")
    framework.assert(map:getBlacktekZone(name) == nil, "Rollback must remove the added zone")
    framework.assert(map.blacktekZonesEnabled == enabled_before, "Rollback must restore format selection")
end)

framework.summary()
