//////////////////////////////////////////////////////////////////////
// This file is part of Remere's Map Editor
//////////////////////////////////////////////////////////////////////

#include "app/main.h"
#include "brushes/zone/zone_brush.h"

#include "map/basemap.h"
#include "map/map.h"
#include "map/tile.h"

bool ZoneBrush::canDraw(BaseMap* map, const Position& position) const {
	if (blacktek_mode) {
		const auto* document_map = dynamic_cast<const Map*>(map);
		return document_map && position.isValid() && document_map->blacktek_zones.isEnabled()
			&& document_map->blacktek_zones.find(document_map->selected_blacktek_zone);
	}
	if (zone_id == 0 || !map || !map->supportsZones()) {
		return false;
	}
	const Tile* tile = map->getTile(position);
	return tile && tile->hasGround();
}

void ZoneBrush::draw(BaseMap* /*map*/, Tile* tile, void* /*parameter*/) {
	if (blacktek_mode) return; // BlackTek geometry is handled by DrawOperations as a document action.
	if (tile && tile->hasGround()) {
		tile->addZone(zone_id);
	}
}

void ZoneBrush::undraw(BaseMap* /*map*/, Tile* tile) {
	if (blacktek_mode) return;
	if (tile) {
		tile->removeZone(zone_id);
	}
}
