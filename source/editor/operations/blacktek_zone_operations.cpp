#include "editor/operations/blacktek_zone_operations.h"

#include "app/main.h"
#include "editor/action.h"
#include "editor/action_queue.h"
#include "editor/editor.h"
#include "game/blacktek_zones.h"
#include "map/map.h"
#include "ui/gui.h"

#include <algorithm>
#include <format>
#include <set>

namespace {

constexpr uint64_t kMaxPaintableCoverage = 1'000'000;

bool translatedPosition(const Position& position, const Position& delta, Position& result, std::string& error) {
	const int64_t x = static_cast<int64_t>(position.x) + delta.x;
	const int64_t y = static_cast<int64_t>(position.y) + delta.y;
	const int64_t z = static_cast<int64_t>(position.z) + delta.z;
	if (x < 0 || x > MAP_MAX_WIDTH || y < 0 || y > MAP_MAX_HEIGHT || z < 0 || z > MAP_MAX_LAYER) {
		error = "Translated zone geometry is outside valid map coordinates.";
		return false;
	}
	result = Position(static_cast<int>(x), static_cast<int>(y), static_cast<int>(z));
	return true;
}

bool writePositions(toml::table& properties, std::span<const Position> positions, std::string& error) {
	toml::array output;
	for (const Position& position : positions) {
		if (!position.isValid()) {
			error = std::format("Zone position {}:{}:{} is outside valid map coordinates.", position.x, position.y, position.z);
			return false;
		}
		output.emplace_back(std::format("{}:{}:{}", position.x, position.y, position.z));
	}
	properties.insert_or_assign("positions", std::move(output));
	return true;
}

bool readCoverage(const BlackTekZoneDocument& document, std::string_view name, std::vector<Position>& positions, std::string& error) {
	const BlackTekZoneRecord* record = document.find(name);
	if (!record) {
		error = std::format("BlackTek zone '{}' no longer exists.", name);
		return false;
	}

	const auto geometry = std::ranges::find(document.geometry(), name, &BlackTekZoneGeometry::name);
	if (geometry == document.geometry().end()) {
		error = std::format("BlackTek zone '{}' has no editable coverage geometry.", name);
		return false;
	}
	if (!geometry->explicit_positions.empty()) {
		positions = geometry->explicit_positions;
		return true;
	}
	if (!geometry->range_start || !geometry->range_end) {
		error = std::format("BlackTek zone '{}' has no position list or range to edit.", name);
		return false;
	}

	const int min_x = std::min(geometry->range_start->x, geometry->range_end->x);
	const int max_x = std::max(geometry->range_start->x, geometry->range_end->x);
	const int min_y = std::min(geometry->range_start->y, geometry->range_end->y);
	const int max_y = std::max(geometry->range_start->y, geometry->range_end->y);
	const int min_z = std::min(geometry->range_start->z, geometry->range_end->z);
	const int max_z = std::max(geometry->range_start->z, geometry->range_end->z);
	const uint64_t width = static_cast<uint64_t>(max_x - min_x + 1);
	const uint64_t height = static_cast<uint64_t>(max_y - min_y + 1);
	const uint64_t floors = static_cast<uint64_t>(max_z - min_z + 1);
	if (width > kMaxPaintableCoverage || height > kMaxPaintableCoverage || floors > kMaxPaintableCoverage
		|| width * height > kMaxPaintableCoverage || width * height * floors > kMaxPaintableCoverage) {
		error = std::format("Zone '{}' range exceeds the {}-tile editing limit. Narrow the range before painting.", name, kMaxPaintableCoverage);
		return false;
	}

	positions.reserve(static_cast<size_t>(width * height * floors));
	for (int z = min_z; z <= max_z; ++z) {
		for (int y = min_y; y <= max_y; ++y) {
			for (int x = min_x; x <= max_x; ++x) {
				positions.emplace_back(x, y, z);
			}
		}
	}
	return true;
}

bool writeRange(toml::table& properties, Position start, Position end, std::string& error) {
	if (!start.isValid() || !end.isValid()) {
		error = "Translated zone range is outside valid map coordinates.";
		return false;
	}
	auto range = properties.get_as<toml::table>("range");
	if (!range) {
		error = "Zone range table is malformed and cannot be translated safely.";
		return false;
	}
	range->insert_or_assign("start", std::format("{}:{}:{}", start.x, start.y, start.z));
	range->insert_or_assign("end", std::format("{}:{}:{}", end.x, end.y, end.z));
	return true;
}

void writeRangeForPositions(toml::table& properties, std::span<const Position> positions) {
	if (positions.empty()) {
		return;
	}
	Position start = positions.front();
	Position end = start;
	for (const Position& position : positions) {
		start.x = std::min(start.x, position.x);
		start.y = std::min(start.y, position.y);
		start.z = std::min(start.z, position.z);
		end.x = std::max(end.x, position.x);
		end.y = std::max(end.y, position.y);
		end.z = std::max(end.z, position.z);
	}
	toml::table range;
	range.insert_or_assign("start", std::format("{}:{}:{}", start.x, start.y, start.z));
	range.insert_or_assign("end", std::format("{}:{}:{}", end.x, end.y, end.z));
	properties.insert_or_assign("range", std::move(range));
}

bool stageChange(Editor& editor, std::unique_ptr<Change> change, std::string& error) {
	if (editor.live_manager.IsLive()) {
		error = "BlackTek zone edits are unavailable during live collaboration until zone metadata synchronization is supported.";
		return false;
	}
	auto action = editor.actionQueue->createAction(ACTION_CHANGE_PROPERTIES);
	action->addChange(std::move(change));
	editor.addAction(std::move(action));
	g_gui.RefreshPalettes(&editor.map);
	g_gui.RefreshView();
	return true;
}

} // namespace

bool BlackTekZoneOperations::apply(Editor& editor, BlackTekZoneDocument document, std::string& error) {
	if (editor.live_manager.IsLive()) {
		error = "BlackTek zone edits are unavailable during live collaboration until zone metadata synchronization is supported.";
		return false;
	}
	if (!document.canSave()) {
		error = "The BlackTek zone files contain unsafe or invalid records. Correct those files before editing or saving.";
		return false;
	}
	if (document.revision() == editor.map.blacktek_zones.revision()
		&& document.isEnabled() == editor.map.blacktek_zones.isEnabled()) {
		return true;
	}
	document.markDirty();
	return stageChange(editor, std::make_unique<Change>(
		std::make_unique<BlackTekZoneDocument>(std::move(document))), error);
}

bool BlackTekZoneOperations::paint(Editor& editor, std::string_view zone_name, std::span<const Position> paint_positions, bool erase, std::string& error) {
	if (editor.live_manager.IsLive()) {
		error = "BlackTek zone edits are unavailable during live collaboration until zone metadata synchronization is supported.";
		return false;
	}
	const BlackTekZoneDocument& document = editor.map.blacktek_zones;
	if (!document.isEnabled()) {
		error = "Enable BlackTek zones for this map before painting their coverage.";
		return false;
	}
	if (!document.canSave()) {
		error = "Unsafe or invalid zone files prevent BlackTek zone edits.";
		return false;
	}
	const BlackTekZoneRecord* existing = document.find(zone_name);
	if (!existing) {
		error = std::format("BlackTek zone '{}' no longer exists.", zone_name);
		return false;
	}
	if (!BlackTekZoneDocument::validateProperties(existing->properties, error)) return false;
	std::vector<Position> coverage;
	if (!readCoverage(document, zone_name, coverage, error)) {
		return false;
	}
	std::set<Position> result(coverage.begin(), coverage.end());
	for (const Position& position : paint_positions) {
		if (!position.isValid()) {
			error = std::format("Position {}:{}:{} is outside valid map coordinates.", position.x, position.y, position.z);
			return false;
		}
		if (erase) {
			result.erase(position);
		} else {
			result.insert(position);
		}
	}
	if (result.empty()) {
		error = "A BlackTek zone cannot have empty coverage. Delete the zone instead.";
		return false;
	}
	if (result.size() > kMaxPaintableCoverage) {
		error = "Zone coverage exceeds the one-million-tile painting limit.";
		return false;
	}
	const std::set<Position> original(coverage.begin(), coverage.end());
	if (result == original) {
		return true;
	}
	std::vector<Position> updated(result.begin(), result.end());
	toml::table properties = existing->properties;
	const auto geometry = std::ranges::find(document.geometry(), zone_name, &BlackTekZoneGeometry::name);
	if (geometry != document.geometry().end() && !geometry->range_start) {
		writeRangeForPositions(properties, coverage);
	}
	if (!writePositions(properties, updated, error)) {
		return false;
	}
	BlackTekZoneRecord replacement = *existing;
	replacement.properties = std::move(properties);
	replacement.dirty = true;
	return stageChange(editor, std::make_unique<Change>(std::move(replacement)), error);
}

bool BlackTekZoneOperations::translatedProperties(const BlackTekZoneDocument& document, std::string_view zone_name, Position delta, toml::table& properties, std::string& error) {
	const BlackTekZoneRecord* existing = document.find(zone_name);
	if (!existing) {
		error = std::format("BlackTek zone '{}' no longer exists.", zone_name);
		return false;
	}
	if (!BlackTekZoneDocument::validateProperties(existing->properties, error)) return false;
	const auto geometry = std::ranges::find(document.geometry(), zone_name, &BlackTekZoneGeometry::name);
	if (geometry == document.geometry().end()) {
		error = std::format("BlackTek zone '{}' has no editable geometry.", zone_name);
		return false;
	}
	properties = existing->properties;
	if (geometry->range_start && geometry->range_end) {
		Position start;
		Position end;
		if (!translatedPosition(*geometry->range_start, delta, start, error)
			|| !translatedPosition(*geometry->range_end, delta, end, error)
			|| !writeRange(properties, start, end, error)) return false;
	}
	if (!geometry->explicit_positions.empty()) {
		std::vector<Position> positions;
		positions.reserve(geometry->explicit_positions.size());
		for (const Position& position : geometry->explicit_positions) {
			Position translated;
			if (!translatedPosition(position, delta, translated, error)) return false;
			positions.push_back(translated);
		}
		if (!writePositions(properties, positions, error)) {
			return false;
		}
	}
	return true;
}
