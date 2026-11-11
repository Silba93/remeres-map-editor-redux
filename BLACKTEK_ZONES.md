# BlackTek zones and spawns

Open **Map → BlackTek Zones and Spawns** to manage the map's BlackTek zones.
The editor reads and writes the sibling `<mapname>-zones` folder recursively.
For example, `world/forgotten.otbm` uses `world/forgotten-zones/*.toml`.
BlackTek uses the name in its server configuration to locate this folder.

Each zone has a unique name, coverage, optional world flags, and optional spawn
behavior. Multiple zones may overlap. These files are independent of classic
spawn XML and Crystal/Canary OTBM 5 zone IDs. Ordinary loading and saving do not
convert or strip legacy flags or spawn files.

## Editing

The manager provides flag and spawn selectors plus the complete zone properties
as TOML. The TOML editor exposes weighted alternatives, schedules, waves, boss
composition, and unfamiliar fields without flattening them into classic spawns.
Apply the dialog to create one undoable edit. Cancel discards its changes.

After applying a selected zone, the zone brush paints its explicit coverage.
Erasing removes coverage. Erasing the final covered tile is refused; delete the
zone in the manager instead. Range-based zones retain their range and spawn
origin when explicit coverage is painted. Tile selection copy/paste continues
to operate on tiles; use the manager's zone transfer controls for zone metadata.

Zone overlays show stored coverage and range bounds. Spawn overlays mark stored
fixed-entry offsets. Relative spawns choose positions dynamically on the server,
so their eventual positions are not represented as fixed markers.

The manager includes all 18 world flags. Monster and boss policies are `fixed`,
`relative`, `staged`, and `triggered`. NPC policies with runtime support are
`fixed` and `relative`. The server currently recognizes `item` but does not
implement its TOML entry loader or spawn dispatch. Such data remains
representable; the editor cannot make the server execute it.

## Coordinates and examples

Zone positions and range endpoints use absolute `x:y:z` map coordinates. Range
endpoints are inclusive, including floors. Creature positions are offsets from
the zone origin. An explicit position list and a range may coexist and must not
be treated as interchangeable: the range also establishes spawning bounds and
the origin. BlackTek derives a bounding box when a spawn zone has only positions.

The manager edits the contents of a zone table. Do not include `[ZoneName]`
around these examples; the manager supplies the selected zone name.

Fixed monster alternatives:

```toml
range = { start = "100:100:7", end = "110:110:7" }
spawn_type = "monster"
policy = "fixed"
passive = true
forced = true
monsters = [
  { position = "2:2:0", interval = 60, list = [
    { name = "Rat", chance = 80 },
    { name = "Cave Rat", chance = 20 }
  ] }
]
```

Staged waves:

```toml
range = { start = "100:100:7", end = "110:110:7" }
spawn_type = "monster"
policy = "staged"
rebootable = true
monsters = [
  { name = "Rat", position = "2:2:0", wave = 1 },
  { name = "Cave Rat", position = "3:3:0", wave = 2 }
]
```

Boss composition:

```toml
range = { start = "100:100:7", end = "110:110:7" }
spawn_type = "boss"
policy = "fixed"
minion_behavior = "despawn_on_master_death"
master = { name = "Demon", position = "5:5:0", interval = 60 }
minions = [
  { name = "Fire Elemental", position = "4:5:0", delay = 1000 }
]
```

For triggered behavior use `policy = "triggered"` and a case-sensitive trigger,
such as `trigger = "Enter"`. Optional `weekdays = ["monday", "friday"]` limits
the schedule; omission means every day. Intervals and cooldown are seconds.
Minion `delay` is passed through as milliseconds in the current server source.
The server stores cooldown in uint16 milliseconds, so values over 65 whole
seconds wrap in the inspected baseline; review them before deployment.

## Preservation and collaboration

Unchanged TOML files retain their original text. Editing a file may reformat it;
unknown values must be retained. Duplicate zone names, unreadable files, malformed
TOML, unsafe paths, and resource limits produce diagnostics rather than silently
dropping data. Resolve load errors before saving zone files. Save As and backup
snapshots include the companion folder.

BlackTek metadata edits are unavailable during live collaboration because the
existing live protocol does not synchronize these documents.

## Lua

Classic `map.spawns` and `tile.spawn` retain their existing meanings. New APIs:

- `map.blacktekZonesEnabled`: whether the BlackTek document is enabled.
- `map:blacktekZoneNames()`: zone names as an array.
- `map:getBlacktekZone(name)`: zone properties as TOML, or `nil`.
- `map:setBlacktekZone(name, propertiesToml)`: add or replace a zone.
- `map:removeBlacktekZone(name)`: remove a zone; returns whether it existed.
- `map:moveBlacktekZone(name, offset)`: translate geometry using a `Position` offset.
- `map:duplicateBlacktekZone(name, newName, offset)`: copy a zone with translated
  geometry, preserving its entry offsets and other properties.

Use `app.transaction` to group zone and tile edits. Script failure rolls back
both; successful edits can be undone together. Edits require the target map to
be the current editor and are refused during live collaboration.

```lua
app.transaction("Add safe area", function()
    app.map:setBlacktekZone("SafeArea", [[
positions = ["100:100:7", "101:100:7"]
flags = ["protection", "no_party"]
]])
end)
```

## Server compatibility

The implementation targets BlackTek master
`32aa93ec8f165b04b842ae2effcdb2dfb0eddf47`. Later server schema changes may require
updates to the editor's vocabulary and validation.
