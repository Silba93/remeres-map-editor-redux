#ifndef RME_EDITOR_OPERATIONS_BLACKTEK_ZONE_OPERATIONS_H_
#define RME_EDITOR_OPERATIONS_BLACKTEK_ZONE_OPERATIONS_H_

#include "map/position.h"
#include "game/blacktek_zones.h"

#include <span>
#include <string>
#include <string_view>

class BlackTekZoneDocument;
class Editor;

class BlackTekZoneOperations {
public:
	[[nodiscard]] static bool apply(Editor& editor, BlackTekZoneDocument document, std::string& error);
	[[nodiscard]] static bool paint(Editor& editor, std::string_view zone, std::span<const Position> positions, bool erase, std::string& error);
	[[nodiscard]] static bool translatedProperties(const BlackTekZoneDocument& document, std::string_view zone, Position delta, toml::table& properties, std::string& error);
};

#endif // RME_EDITOR_OPERATIONS_BLACKTEK_ZONE_OPERATIONS_H_
