//////////////////////////////////////////////////////////////////////
// This file is part of Remere's Map Editor
//////////////////////////////////////////////////////////////////////

#include "game/blacktek_zones.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <ranges>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace {
	constexpr size_t MAX_ZONE_FILES = 4096;
	constexpr size_t MAX_FILE_BYTES = 16 * 1024 * 1024;
	constexpr size_t MAX_TOTAL_BYTES = 64 * 1024 * 1024;
	constexpr size_t MAX_ZONES = 100'000;
	constexpr size_t MAX_POSITIONS_PER_ZONE = 1'000'000;

	constexpr std::array<std::string_view, 18> ZONE_FLAGS = {
		"protection", "nopvp", "pvp", "nologout", "no_experience", "no_trading",
		"no_transaction", "no_party", "no_drop", "no_death", "no_death_penalty",
		"no_equipment_loss", "no_summons", "ghost", "no_pathfinding", "no_walk",
		"no_monsters", "no_npcs"
	};
	constexpr std::array<std::string_view, 4> SPAWN_TYPES = { "monster", "boss", "npc", "item" };
	constexpr std::array<std::string_view, 4> SPAWN_POLICIES = { "fixed", "relative", "staged", "triggered" };
	constexpr std::array<std::string_view, 23> SPAWN_TRIGGERS = {
		"Spawn", "Enter", "Leave", "Death", "Despawn", "Login", "Logout", "Use",
		"Remove", "Transform", "Speak", "Look", "Summon", "Equip", "DeEquip", "Attack",
		"Defend", "Idle", "GainSkull", "LoseSkull", "Kill", "EnterBattle", "LeaveBattle"
	};

	std::string pathText(const std::filesystem::path& path) {
		return path.string();
	}

	std::string formatTable(const toml::table& table) {
		std::ostringstream stream;
		stream << table;
		return stream.str();
	}

	// Conservative accounting for undo limits, including nested entries and positions.
	// Counting the tree avoids allocating a serialized copy just to estimate its size.
	size_t nodeMemorySize(const toml::node& node) noexcept {
		size_t size = sizeof(toml::table);
		if (const auto* text = node.as_string()) {
			size += text->get().capacity();
		} else if (const auto* array = node.as_array()) {
			size += array->size() * sizeof(void*) * 2;
			for (const auto& value : *array) size += nodeMemorySize(value);
		} else if (const auto* table = node.as_table()) {
			for (const auto& [key, value] : *table) {
				size += sizeof(toml::key) + key.str().size() + sizeof(void*) * 4;
				size += nodeMemorySize(value);
			}
		}
		return size;
	}

	bool isWithin(const std::filesystem::path& root, const std::filesystem::path& child) {
		const auto relative = child.lexically_relative(root);
		if (relative.empty() || relative.is_absolute()) {
			return false;
		}
		return std::ranges::none_of(relative, [](const auto& component) { return component == ".."; });
	}

	std::optional<Position> parsePosition(const toml::node& node) {
		const auto value = node.value<std::string>();
		if (!value) {
			return std::nullopt;
		}
		const std::string_view text = *value;
		const size_t first = text.find(':');
		const size_t second = first == std::string_view::npos ? first : text.find(':', first + 1);
		if (first == std::string_view::npos || second == std::string_view::npos || text.find(':', second + 1) != std::string_view::npos) {
			return std::nullopt;
		}
		int values[3]{};
		const std::array<std::string_view, 3> parts = { text.substr(0, first), text.substr(first + 1, second - first - 1), text.substr(second + 1) };
		for (size_t index = 0; index < parts.size(); ++index) {
			const auto [end, error] = std::from_chars(parts[index].data(), parts[index].data() + parts[index].size(), values[index]);
			if (error != std::errc{} || end != parts[index].data() + parts[index].size() || values[index] < 0) {
				return std::nullopt;
			}
		}
		if (values[0] > 65535 || values[1] > 65535 || values[2] > 255) return std::nullopt;
		return Position{values[0], values[1], values[2]};
	}

	std::optional<Position> parsePosition(const toml::table& table, std::string_view key) {
		const auto* node = table.get(key);
		return node ? parsePosition(*node) : std::nullopt;
	}

	std::string safeStem(std::string_view name) {
		std::string stem;
		stem.reserve(name.size());
		for (const unsigned char ch : name) {
			if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
				(ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
				stem.push_back(static_cast<char>(ch));
			} else if (!stem.empty() && stem.back() != '_') {
				stem.push_back('_');
			}
		}
		while (!stem.empty() && (stem.back() == '_' || stem.back() == '.')) {
			stem.pop_back();
		}
		if (stem.empty() || stem == "." || stem == "..") {
			stem = "zone";
		}
		return stem;
	}

	std::filesystem::path uniqueSibling(const std::filesystem::path& target, std::string_view suffix) {
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		for (uint32_t attempt = 0; attempt < 1000; ++attempt) {
			auto candidate = target.parent_path() /
				std::format("{}.{}.{}.{}", target.filename().string(), suffix, stamp, attempt);
			std::error_code ec;
			if (!std::filesystem::exists(candidate, ec) && !std::filesystem::is_symlink(candidate, ec)) {
				return candidate;
			}
		}
		return {};
	}

	bool containsSymlink(const std::filesystem::path& root, std::string& error) {
		std::error_code ec;
		if (std::filesystem::is_symlink(std::filesystem::symlink_status(root, ec))) {
			error = "BlackTek zone directory cannot be a symbolic link";
			return true;
		}
		if (ec || !std::filesystem::exists(root, ec)) {
			return false;
		}
		for (std::filesystem::recursive_directory_iterator iter(root, ec), end; iter != end; iter.increment(ec)) {
			if (ec) {
				error = std::format("Cannot inspect BlackTek zone directory: {}", ec.message());
				return true;
			}
			if (iter->is_symlink(ec) || ec) {
				error = "BlackTek zone directory contains a symbolic link";
				return true;
			}
		}
		return false;
	}

	void appendEntryPositions(
		const toml::table& properties,
		std::string_view key,
		const Position& origin,
		std::vector<Position>& output
	) {
		const auto* entries = properties.get_as<toml::array>(key);
		if (!entries) {
			return;
		}
		for (const auto& entry : *entries) {
			const auto* table = entry.as_table();
			if (!table) {
				continue;
			}
			const auto position = parsePosition(*table, "position");
			if (position && output.size() < MAX_POSITIONS_PER_ZONE) {
				output.emplace_back(origin.x + position->x, origin.y + position->y, origin.z + position->z);
			}
		}
	}

	bool hasEditableGeometry(const toml::table& properties) {
		const auto* positions_node = properties.get("positions");
		bool has_positions = false;
		bool positions_valid = true;
		if (const auto* positions = positions_node ? positions_node->as_array() : nullptr) {
			if (positions->size() > MAX_POSITIONS_PER_ZONE) positions_valid = false;
			for (const auto& position : *positions) {
				if (parsePosition(position)) has_positions = true;
				else positions_valid = false;
			}
		}
		if (positions_node && (!positions_node->is_array() || !positions_valid)) return false;
		bool has_range = false;
		if (const auto* range = properties.get_as<toml::table>("range")) {
			const auto start = range->get("start");
			const auto end = range->get("end");
			has_range = start && end && parsePosition(*start).has_value() && parsePosition(*end).has_value();
		}
		if (properties.contains("range") && !has_range) return false;
		return (has_positions && positions_valid) || has_range;
	}

	std::string caseFoldedPathKey(const std::filesystem::path& path) {
		std::string key = path.generic_string();
		std::ranges::transform(key, key.begin(), [](unsigned char ch) {
			return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
		});
		return key;
	}

	std::filesystem::path uniqueZonePath(std::string_view name, const std::vector<BlackTekZoneRecord>& records, const std::vector<std::filesystem::path>& source_paths) {
		const std::string stem = safeStem(name);
		for (uint32_t suffix = 1; suffix < 100'000; ++suffix) {
			const std::string candidate_name = suffix == 1
				? std::format("{}.toml", stem)
				: std::format("{}-{}.toml", stem, suffix);
			const std::filesystem::path candidate(candidate_name);
			const auto candidate_key = caseFoldedPathKey(candidate);
			const bool path_used =
				std::ranges::any_of(source_paths, [&](const auto& path) { return caseFoldedPathKey(path) == candidate_key; }) ||
				std::ranges::any_of(records, [&](const auto& record) { return caseFoldedPathKey(record.source_file) == candidate_key; });
			if (!path_used) return candidate;
		}
		return {};
	}

	bool writeTable(const std::filesystem::path& path, const toml::table& table, std::string& error) {
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		if (!stream) {
			error = std::format("Cannot open staged BlackTek zone file for writing: {}", pathText(path));
			return false;
		}
		stream << table;
		stream.flush();
		if (!stream) {
			error = std::format("Cannot write staged BlackTek zone file: {}", pathText(path));
			return false;
		}
		return true;
	}
}

BlackTekZoneSaveTransaction::BlackTekZoneSaveTransaction(BlackTekZoneSaveTransaction&& other) noexcept {
	*this = std::move(other);
}

BlackTekZoneSaveTransaction& BlackTekZoneSaveTransaction::operator=(BlackTekZoneSaveTransaction&& other) noexcept {
	if (this == &other) {
		return *this;
	}
	std::string ignored;
	if (!finalized_) {
		rollback(ignored);
	}
	target_directory_ = std::move(other.target_directory_);
	staged_directory_ = std::move(other.staged_directory_);
	backup_directory_ = std::move(other.backup_directory_);
	target_existed_ = other.target_existed_;
	recovery_sentinel_ = other.recovery_sentinel_;
	committed_ = other.committed_;
	finalized_ = other.finalized_;
	other.finalized_ = true;
	return *this;
}

BlackTekZoneSaveTransaction::~BlackTekZoneSaveTransaction() {
	if (!finalized_) {
		std::string ignored;
		rollback(ignored);
	}
}

bool BlackTekZoneSaveTransaction::prepared() const noexcept {
	return !target_directory_.empty() && !staged_directory_.empty() && !finalized_;
}

bool BlackTekZoneSaveTransaction::commit(std::string& error) {
	if (!prepared() || committed_) {
		error = "BlackTek zone save transaction is not ready to commit";
		return false;
	}

	std::error_code ec;
	if (std::filesystem::is_symlink(std::filesystem::symlink_status(target_directory_, ec))) {
		error = "BlackTek zone destination cannot be a symbolic link";
		return false;
	}
	if (ec && ec != std::errc::no_such_file_or_directory) {
		error = std::format("Cannot inspect BlackTek zone destination: {}", ec.message());
		return false;
	}
	const bool target_exists_now = std::filesystem::exists(target_directory_, ec);
	if (ec || target_exists_now != target_existed_) {
		error = "BlackTek zone destination changed while the save was being prepared";
		return false;
	}
	if (target_existed_) {
		if (std::filesystem::exists(backup_directory_, ec) || ec) {
			error = std::format("BlackTek zone recovery path already exists: {}", pathText(backup_directory_));
			return false;
		}
		std::filesystem::rename(target_directory_, backup_directory_, ec);
		if (ec) {
			error = std::format("Cannot preserve the current BlackTek zone directory: {}", ec.message());
			return false;
		}
	}
	else if (recovery_sentinel_) {
		std::filesystem::create_directory(backup_directory_, ec);
		if (ec) {
		error = std::format("Cannot create BlackTek zone recovery sentinel: {}", ec.message());
			return false;
		}
	}
	std::filesystem::rename(staged_directory_, target_directory_, ec);
	if (ec) {
		error = std::format("Cannot install staged BlackTek zone directory: {}", ec.message());
		if (target_existed_) {
			std::error_code restore_error;
			std::filesystem::rename(backup_directory_, target_directory_, restore_error);
			if (restore_error) {
				error += std::format("; restoring the original directory also failed: {}", restore_error.message());
			}
		} else if (recovery_sentinel_) {
			std::error_code cleanup_error;
			std::filesystem::remove(backup_directory_, cleanup_error);
		}
		return false;
	}
	committed_ = true;
	return true;
}

bool BlackTekZoneSaveTransaction::rollback(std::string& error) noexcept {
	if (finalized_) {
		return true;
	}
	std::error_code ec;
	if (committed_) {
		std::filesystem::remove_all(target_directory_, ec);
		if (ec) {
			error = std::format("Cannot remove the staged BlackTek zone directory during rollback: {}", ec.message());
			return false;
		}
		if (target_existed_) {
			std::filesystem::rename(backup_directory_, target_directory_, ec);
			if (ec) {
				error = std::format("Cannot restore the original BlackTek zone directory: {}", ec.message());
				return false;
			}
		} else if (recovery_sentinel_) {
			std::filesystem::remove(backup_directory_, ec);
			if (ec) {
				error = std::format("Cannot remove BlackTek zone recovery sentinel: {}", ec.message());
				return false;
			}
		}
	} else if (!staged_directory_.empty()) {
		std::filesystem::remove_all(staged_directory_, ec);
		if (ec) {
			error = std::format("Cannot remove staged BlackTek zone directory: {}", ec.message());
			return false;
		}
	}
	finalized_ = true;
	return true;
}

void BlackTekZoneSaveTransaction::finalize(bool retain_recovery_backup) noexcept {
	if (finalized_) {
		return;
	}
	std::error_code ec;
	if (committed_ && (target_existed_ || recovery_sentinel_) && !retain_recovery_backup) {
		std::filesystem::remove_all(backup_directory_, ec);
	}
	if (!staged_directory_.empty()) {
		std::filesystem::remove_all(staged_directory_, ec);
	}
	finalized_ = true;
}

bool BlackTekZoneDocument::load(const std::filesystem::path& directory) {
	BlackTekZoneDocument loaded;
	loaded.source_directory_ = directory;
	std::error_code ec;
	const auto status = std::filesystem::symlink_status(directory, ec);
	if (ec == std::errc::no_such_file_or_directory || status.type() == std::filesystem::file_type::not_found) {
		*this = std::move(loaded);
		return true;
	}
	if (ec || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
		loaded.safe_to_save_ = false;
		loaded.diagnostics_.push_back(std::format("BlackTek zone path is not a safe directory: {}", pathText(directory)));
		*this = std::move(loaded);
		return false;
	}

	const auto root = std::filesystem::weakly_canonical(directory, ec);
	if (ec) {
		loaded.safe_to_save_ = false;
		loaded.diagnostics_.push_back(std::format("Cannot resolve BlackTek zone directory: {}", ec.message()));
		*this = std::move(loaded);
		return false;
	}
	loaded.source_directory_ = root;
	loaded.enabled_ = true;

	std::string symlink_error;
	if (containsSymlink(root, symlink_error)) {
		loaded.safe_to_save_ = false;
		loaded.diagnostics_.push_back(std::move(symlink_error));
		*this = std::move(loaded);
		return false;
	}

	size_t total_bytes = 0;
	std::vector<std::filesystem::path> toml_paths;
	for (std::filesystem::recursive_directory_iterator iter(root, ec), end; iter != end; iter.increment(ec)) {
		if (ec) {
			loaded.safe_to_save_ = false;
			loaded.diagnostics_.push_back(std::format("Cannot scan BlackTek zone directory: {}", ec.message()));
			*this = std::move(loaded);
			return false;
		}
		if (!iter->is_regular_file(ec) || ec || iter->path().extension() != ".toml") {
			continue;
		}
		if (toml_paths.size() >= MAX_ZONE_FILES) {
			loaded.safe_to_save_ = false;
			loaded.diagnostics_.push_back("BlackTek zone directory exceeds the file count limit");
			*this = std::move(loaded);
			return false;
		}
		const auto canonical = std::filesystem::canonical(iter->path(), ec);
		if (ec || !isWithin(root, canonical)) {
			loaded.safe_to_save_ = false;
			loaded.diagnostics_.push_back("BlackTek zone file resolves outside its map directory");
			*this = std::move(loaded);
			return false;
		}
		toml_paths.push_back(canonical);
	}
	if (ec) {
		loaded.safe_to_save_ = false;
		loaded.diagnostics_.push_back(std::format("Cannot scan BlackTek zone directory: {}", ec.message()));
		*this = std::move(loaded);
		return false;
	}
	std::ranges::sort(toml_paths);

	std::unordered_set<std::string> names;
	std::unordered_set<std::string> source_path_keys;
	for (const auto& path : toml_paths) {
		const auto file_size = std::filesystem::file_size(path, ec);
		if (ec || file_size > MAX_FILE_BYTES || total_bytes > MAX_TOTAL_BYTES - file_size) {
			loaded.safe_to_save_ = false;
			loaded.diagnostics_.push_back("BlackTek zone files exceed the configured size limits");
			*this = std::move(loaded);
			return false;
		}
		total_bytes += static_cast<size_t>(file_size);
		std::ifstream stream(path, std::ios::binary);
		if (!stream) {
			loaded.safe_to_save_ = false;
			loaded.diagnostics_.push_back(std::format("Cannot read BlackTek zone file: {}", pathText(path)));
			*this = std::move(loaded);
			return false;
		}
		std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		toml::table root_table;
		try {
			root_table = toml::parse(text, pathText(path));
		} catch (const toml::parse_error& parse_error) {
			loaded.safe_to_save_ = false;
			loaded.diagnostics_.push_back(std::format("Cannot parse {}: {}", pathText(path), parse_error.description()));
			*this = std::move(loaded);
			return false;
		}

		SourceFile source;
		source.relative_path = path.lexically_relative(root);
		if (!source_path_keys.insert(caseFoldedPathKey(source.relative_path)).second) {
			loaded.safe_to_save_ = false;
			loaded.diagnostics_.push_back("BlackTek zone filenames collide on case-insensitive filesystems");
			*this = std::move(loaded);
			return false;
		}
		source.root = root_table;
		source.baseline_root = root_table;
		source.original_text = std::move(text);
		source.baseline_exists = true;
		loaded.source_files_.push_back(std::move(source));
		for (const auto& [key, node] : root_table) {
			const auto* zone_table = node.as_table();
			if (!zone_table) {
				continue;
			}
			const std::string name(key.str());
			if (name.empty() || !names.insert(name).second) {
				loaded.safe_to_save_ = false;
				loaded.diagnostics_.push_back(std::format("Duplicate or empty BlackTek zone name: {}", name));
				continue;
			}
			if (loaded.records_.size() >= MAX_ZONES) {
				loaded.safe_to_save_ = false;
				loaded.diagnostics_.push_back("BlackTek zone directory exceeds the zone count limit");
				*this = std::move(loaded);
				return false;
			}
			loaded.records_.push_back({ name, source.relative_path, *zone_table, false });
		}
	}
	loaded.rebuildGeometry();
	*this = std::move(loaded);
	return safe_to_save_;
}

void BlackTekZoneDocument::setEnabled(bool enabled) noexcept {
	if (enabled_ != enabled) {
		enabled_ = enabled;
		markModified();
	}
}

void BlackTekZoneDocument::markDirty() noexcept {
	markModified();
}

size_t BlackTekZoneDocument::recordMemorySize(const BlackTekZoneRecord& record) noexcept {
	return sizeof(record) + record.name.capacity()
		+ record.source_file.native().capacity() * sizeof(std::filesystem::path::value_type)
		+ nodeMemorySize(record.properties);
}

size_t BlackTekZoneDocument::estimatedMemorySize() const noexcept {
	size_t size = sizeof(*this) + records_.capacity() * sizeof(BlackTekZoneRecord)
		+ source_files_.capacity() * sizeof(SourceFile) + geometry_.capacity() * sizeof(BlackTekZoneGeometry);
	for (const auto& record : records_) size += recordMemorySize(record);
	for (const auto& source : source_files_) {
		size += source.relative_path.native().capacity() * sizeof(std::filesystem::path::value_type)
			+ source.original_text.capacity() + nodeMemorySize(source.root) + nodeMemorySize(source.baseline_root);
	}
	for (const auto& geometry : geometry_) {
		size += geometry.name.capacity() + (geometry.explicit_positions.capacity() + geometry.fixed_entry_positions.capacity()) * sizeof(Position);
	}
	for (const auto& diagnostic : diagnostics_) size += sizeof(std::string) + diagnostic.capacity();
	return size;
}

void BlackTekZoneDocument::markSaved() {
	for (auto& record : records_) {
		record.dirty = false;
	}
	for (auto& source : source_files_) {
		if (source.dirty || formatTable(source.root) != formatTable(source.baseline_root)) {
			source.baseline_root = source.root;
			source.baseline_exists = !source.root.empty();
			source.original_text = source.baseline_exists ? formatTable(source.root) : std::string{};
		}
		source.dirty = false;
	}
}

void BlackTekZoneDocument::markSaved(const std::filesystem::path& directory) {
	markSaved();
	std::error_code ec;
	source_directory_ = std::filesystem::weakly_canonical(directory, ec);
	if (ec) source_directory_ = directory.lexically_normal();
}

void BlackTekZoneDocument::inheritSaveBaselineFrom(const BlackTekZoneDocument& other) {
	source_directory_ = other.source_directory_;
	for (auto& source : source_files_) {
		const auto previous = std::ranges::find_if(other.source_files_, [&](const auto& candidate) {
			return caseFoldedPathKey(candidate.relative_path) == caseFoldedPathKey(source.relative_path);
		});
		if (previous == other.source_files_.end()) {
			source.baseline_root = toml::table{};
			source.original_text.clear();
			source.baseline_exists = false;
			source.dirty = !source.root.empty();
		} else {
			source.baseline_root = previous->baseline_root;
			source.original_text = previous->original_text;
			source.baseline_exists = previous->baseline_exists;
			source.dirty = formatTable(source.root) != formatTable(source.baseline_root);
		}
	}
	for (const auto& previous : other.source_files_) {
		const auto current = std::ranges::find_if(source_files_, [&](const auto& candidate) {
			return caseFoldedPathKey(candidate.relative_path) == caseFoldedPathKey(previous.relative_path);
		});
		if (current == source_files_.end()) {
			SourceFile tombstone;
			tombstone.relative_path = previous.relative_path;
			tombstone.baseline_root = previous.baseline_root;
			tombstone.original_text = previous.original_text;
			tombstone.baseline_exists = previous.baseline_exists;
			tombstone.dirty = previous.baseline_exists;
			source_files_.push_back(std::move(tombstone));
		}
	}
}

const BlackTekZoneRecord* BlackTekZoneDocument::find(std::string_view name) const noexcept {
	const auto iter = std::ranges::find(records_, name, &BlackTekZoneRecord::name);
	return iter == records_.end() ? nullptr : &*iter;
}

bool BlackTekZoneDocument::add(std::string name, toml::table properties, std::string& error) {
	if (name.empty() || find(name)) {
		error = name.empty() ? "Zone name cannot be empty" : "A zone with that name already exists";
		return false;
	}
	if (!hasEditableGeometry(properties)) {
		error = "BlackTek zones require a non-empty valid positions list or a valid range";
		return false;
	}
	return addDraft(std::move(name), std::move(properties), error);
}

bool BlackTekZoneDocument::addDraft(std::string name, toml::table properties, std::string& error) {
	std::vector<std::filesystem::path> source_paths;
	source_paths.reserve(source_files_.size());
	for (const auto& file : source_files_) source_paths.push_back(file.relative_path);
	const auto unique_path = uniqueZonePath(name, records_, source_paths);
	if (unique_path.empty()) {
		error = "Cannot allocate a unique BlackTek zone filename";
		return false;
	}
	const auto source = std::ranges::find(source_files_, unique_path, &SourceFile::relative_path);
	if (source == source_files_.end()) {
		source_files_.push_back({ unique_path, toml::table{}, toml::table{}, {}, false, true });
	} else {
		source->dirty = true;
	}
	records_.push_back({ std::move(name), unique_path, std::move(properties), true });
	auto& target_file = *std::ranges::find(source_files_, unique_path, &SourceFile::relative_path);
	target_file.root.insert_or_assign(records_.back().name, records_.back().properties);
	enabled_ = true;
	markModified();
	rebuildGeometry();
	return true;
}

bool BlackTekZoneDocument::add(BlackTekZoneRecord record, std::string& error) {
	if (record.name.empty() || find(record.name)) {
		error = record.name.empty() ? "Zone name cannot be empty" : "A zone with that name already exists";
		return false;
	}
	if (!hasEditableGeometry(record.properties)) {
		error = "BlackTek zones require a non-empty valid positions list or a valid range";
		return false;
	}
	if (record.source_file.empty()) {
		record.source_file = safeStem(record.name) + ".toml";
	}
	if (record.source_file.is_absolute() || std::ranges::any_of(record.source_file, [](const auto& part) { return part == ".."; })) {
		error = "BlackTek zone source file path escapes its map directory";
		return false;
	}
	const auto source = std::ranges::find_if(source_files_, [&](const auto& file) {
		return caseFoldedPathKey(file.relative_path) == caseFoldedPathKey(record.source_file);
	});
	if (source == source_files_.end()) {
		source_files_.push_back({ record.source_file, toml::table{}, toml::table{}, {}, false, true });
	} else {
		source->dirty = true;
	}
	const auto source_match = std::ranges::find_if(source_files_, [&](const auto& file) {
		return caseFoldedPathKey(file.relative_path) == caseFoldedPathKey(record.source_file);
	});
	auto& source_file = *source_match;
	record.source_file = source_file.relative_path;
	record.dirty = true;
	source_file.root.insert_or_assign(record.name, record.properties);
	records_.push_back(std::move(record));
	markModified();
	rebuildGeometry();
	return true;
}

bool BlackTekZoneDocument::replace(std::string_view name, toml::table properties, std::string& error) {
	const auto record = std::ranges::find(records_, name, &BlackTekZoneRecord::name);
	if (record == records_.end()) {
		error = "Zone no longer exists";
		return false;
	}
	if (!validateProperties(properties, error)) {
		return false;
	}
	const auto source = std::ranges::find(source_files_, record->source_file, &SourceFile::relative_path);
	if (source == source_files_.end()) {
		error = "BlackTek zone source file is missing";
		return false;
	}
	record->properties = std::move(properties);
	record->dirty = true;
	source->root.insert_or_assign(record->name, record->properties);
	source->dirty = true;
	markModified();
	rebuildGeometry();
	return true;
}

bool BlackTekZoneDocument::rename(std::string_view name, std::string new_name, std::string& error) {
	const auto record = std::ranges::find(records_, name, &BlackTekZoneRecord::name);
	if (record == records_.end() || new_name.empty()) {
		error = record == records_.end() ? "Zone no longer exists" : "Zone name cannot be empty";
		return false;
	}
	if (find(new_name)) {
		error = "A zone with that name already exists";
		return false;
	}
	const auto source = std::ranges::find(source_files_, record->source_file, &SourceFile::relative_path);
	if (source == source_files_.end()) {
		error = "BlackTek zone source file is missing";
		return false;
	}
	const std::string old_name = record->name;
	toml::table properties = record->properties;
	source->root.erase(old_name);
	source->root.insert_or_assign(new_name, properties);
	source->dirty = true;
	record->name = std::move(new_name);
	record->dirty = true;
	markModified();
	rebuildGeometry();
	return true;
}

bool BlackTekZoneDocument::remove(std::string_view name) {
	const auto record = std::ranges::find(records_, name, &BlackTekZoneRecord::name);
	if (record == records_.end()) {
		return false;
	}
	const auto source = std::ranges::find(source_files_, record->source_file, &SourceFile::relative_path);
	if (source != source_files_.end()) {
		source->root.erase(record->name);
		source->dirty = true;
	}
	records_.erase(record);
	markModified();
	rebuildGeometry();
	return true;
}

void BlackTekZoneDocument::rebuildGeometry() {
	geometry_.clear();
	geometry_.reserve(records_.size());
	for (const auto& record : records_) {
		BlackTekZoneGeometry data;
		data.name = record.name;
		if (const auto* range = record.properties.get_as<toml::table>("range")) {
			if (const auto* start = range->get("start")) data.range_start = parsePosition(*start);
			if (const auto* end = range->get("end")) data.range_end = parsePosition(*end);
		}
		if (const auto* positions = record.properties.get_as<toml::array>("positions")) {
			if (positions->size() <= MAX_POSITIONS_PER_ZONE) {
				data.explicit_positions.reserve(positions->size());
				for (const auto& position_node : *positions) {
					if (const auto position = parsePosition(position_node)) {
						data.explicit_positions.push_back(*position);
					}
				}
			}
		}
		if (!data.range_start && !data.explicit_positions.empty()) {
			Position minimum = data.explicit_positions.front();
			for (const auto& position : data.explicit_positions) {
				minimum.x = std::min(minimum.x, position.x);
				minimum.y = std::min(minimum.y, position.y);
				minimum.z = std::min(minimum.z, position.z);
			}
			data.spawn_origin = minimum;
		}
		else if (data.range_start) {
			data.spawn_origin = data.range_start;
		}
		if (data.spawn_origin) {
			const auto* policy = record.properties.get("policy");
			const auto policy_name = policy ? policy->value<std::string>() : std::nullopt;
			if (!policy_name || *policy_name != "relative") {
				appendEntryPositions(record.properties, "monsters", *data.spawn_origin, data.fixed_entry_positions);
				appendEntryPositions(record.properties, "npcs", *data.spawn_origin, data.fixed_entry_positions);
				appendEntryPositions(record.properties, "minions", *data.spawn_origin, data.fixed_entry_positions);
				if (const auto* master = record.properties.get_as<toml::table>("master")) {
					if (const auto position = parsePosition(*master, "position")) {
						data.fixed_entry_positions.emplace_back(data.spawn_origin->x + position->x, data.spawn_origin->y + position->y, data.spawn_origin->z + position->z);
					}
				}
			}
		}
		std::ranges::sort(data.explicit_positions);
		data.explicit_positions.erase(std::unique(data.explicit_positions.begin(), data.explicit_positions.end()), data.explicit_positions.end());
		std::ranges::sort(data.fixed_entry_positions);
		data.fixed_entry_positions.erase(std::unique(data.fixed_entry_positions.begin(), data.fixed_entry_positions.end()), data.fixed_entry_positions.end());
		geometry_.push_back(std::move(data));
	}
}

void BlackTekZoneDocument::markModified() noexcept {
	++revision_;
}

std::string BlackTekZoneDocument::formatProperties(const toml::table& properties) {
	return formatTable(properties);
}

bool BlackTekZoneDocument::parseProperties(std::string_view text, toml::table& properties, std::string& error) {
	try {
		properties = toml::parse(text);
		return true;
	} catch (const toml::parse_error& parse_error) {
		error = std::string(parse_error.description());
		return false;
	}
}

bool BlackTekZoneDocument::validateProperties(const toml::table& properties, std::string& error) {
	if (!hasEditableGeometry(properties)) {
		error = "BlackTek zones require a non-empty valid positions list or a valid range";
		return false;
	}
	error.clear();
	return true;
}

std::span<const std::string_view> BlackTekZoneDocument::flagNames() noexcept { return ZONE_FLAGS; }
std::span<const std::string_view> BlackTekZoneDocument::spawnTypes() noexcept { return SPAWN_TYPES; }
std::span<const std::string_view> BlackTekZoneDocument::policies() noexcept { return SPAWN_POLICIES; }
std::span<const std::string_view> BlackTekZoneDocument::triggerNames() noexcept { return SPAWN_TRIGGERS; }

std::optional<BlackTekZoneSaveTransaction> BlackTekZoneDocument::stageSave(
	const std::filesystem::path& directory,
	std::string& error,
	bool retain_recovery_backup
) const {
	if (!safe_to_save_) {
		error = "BlackTek zone document contains unreadable or unsafe source data; refusing to overwrite it";
		return std::nullopt;
	}
	if (directory.empty()) {
		error = "BlackTek zone directory path is empty";
		return std::nullopt;
	}

	std::error_code ec;
	const auto parent = directory.parent_path().empty() ? std::filesystem::current_path(ec) : directory.parent_path();
	if (ec) {
		error = std::format("Cannot resolve BlackTek zone parent directory: {}", ec.message());
		return std::nullopt;
	}
	std::filesystem::create_directories(parent, ec);
	if (ec) {
		error = std::format("Cannot create BlackTek zone parent directory: {}", ec.message());
		return std::nullopt;
	}
	const auto target = parent / directory.filename();
	if (std::filesystem::is_symlink(std::filesystem::symlink_status(target, ec))) {
		error = "BlackTek zone destination cannot be a symbolic link";
		return std::nullopt;
	}
	if (ec && ec != std::errc::no_such_file_or_directory) {
		error = std::format("Cannot inspect BlackTek zone destination: {}", ec.message());
		return std::nullopt;
	}
	const bool target_exists = std::filesystem::exists(target, ec);
	if (ec || (target_exists && !std::filesystem::is_directory(target, ec))) {
		error = "BlackTek zone destination is not a directory";
		return std::nullopt;
	}
	if (containsSymlink(target, error)) {
		return std::nullopt;
	}
	const auto recovery_path = std::filesystem::path(target.string() + "~");
	if (std::filesystem::exists(recovery_path, ec) || ec) {
		error = std::format("A BlackTek zone recovery directory already exists: {}", pathText(recovery_path));
		return std::nullopt;
	}

	BlackTekZoneSaveTransaction transaction;
	transaction.target_directory_ = target;
	transaction.staged_directory_ = uniqueSibling(target, "rme-stage");
	transaction.backup_directory_ = recovery_path;
	transaction.target_existed_ = target_exists;
	transaction.recovery_sentinel_ = retain_recovery_backup && !target_exists;
	if (transaction.staged_directory_.empty() || transaction.backup_directory_.empty()) {
		error = "Cannot allocate temporary BlackTek zone paths";
		return std::nullopt;
	}
	if (target_exists) {
		std::filesystem::copy(target, transaction.staged_directory_, std::filesystem::copy_options::recursive, ec);
		if (ec) {
			error = std::format("Cannot stage BlackTek zone directory: {}", ec.message());
			return std::nullopt;
		}
	} else if (!std::filesystem::create_directory(transaction.staged_directory_, ec) || ec) {
		error = std::format("Cannot create staged BlackTek zone directory: {}", ec.message());
		return std::nullopt;
	}

	std::unordered_map<std::string, toml::table> output_files;
	std::error_code canonical_error;
	const auto target_parent = std::filesystem::weakly_canonical(parent, canonical_error);
	if (canonical_error) {
		error = std::format("Cannot resolve BlackTek zone destination parent: {}", canonical_error.message());
		return std::nullopt;
	}
	const auto normalized_target = target_parent / directory.filename();
	const bool saving_as = source_directory_.empty() || source_directory_.lexically_normal() != normalized_target.lexically_normal();
	std::unordered_set<std::string> known_source_paths;
	for (const auto& source : source_files_) known_source_paths.insert(caseFoldedPathKey(source.relative_path));
	if (target_exists) {
		for (std::filesystem::recursive_directory_iterator iter(target, ec), end; iter != end; iter.increment(ec)) {
			if (ec) {
				error = std::format("Cannot inspect existing BlackTek zone destination: {}", ec.message());
				return std::nullopt;
			}
			if (iter->is_regular_file(ec) && !ec && iter->path().extension() == ".toml") {
				const auto relative = iter->path().lexically_relative(target);
				if (!known_source_paths.contains(caseFoldedPathKey(relative))) {
					error = std::format("BlackTek zone destination contains an untracked TOML file: {}", pathText(relative));
					return std::nullopt;
				}
			}
		}
		if (ec) {
			error = std::format("Cannot inspect existing BlackTek zone destination: {}", ec.message());
			return std::nullopt;
		}
	}
	for (const auto& source : source_files_) {
		const std::string key = source.relative_path.generic_string();
		if (key.empty() || source.relative_path.is_absolute() || std::ranges::any_of(source.relative_path, [](const auto& part) { return part == ".."; })) {
			error = "BlackTek zone source file path escapes its map directory";
			return std::nullopt;
		}
		const auto disk_path = target / source.relative_path;
		const bool disk_file_exists = std::filesystem::exists(disk_path, ec);
		if (ec) {
			error = std::format("Cannot inspect BlackTek zone source file: {}", ec.message());
			return std::nullopt;
		}
		if (!saving_as) {
			if (disk_file_exists != source.baseline_exists) {
				error = std::format("BlackTek zone file changed on disk since it was loaded: {}", pathText(disk_path));
				return std::nullopt;
			}
			if (disk_file_exists) {
				std::ifstream current(disk_path, std::ios::binary);
				if (!current) {
					error = std::format("Cannot read BlackTek zone file before save: {}", pathText(disk_path));
					return std::nullopt;
				}
				const std::string current_text((std::istreambuf_iterator<char>(current)), std::istreambuf_iterator<char>());
				if (current_text != source.original_text) {
					error = std::format("BlackTek zone file changed on disk since it was loaded: {}", pathText(disk_path));
					return std::nullopt;
				}
			}
		} else {
			if (disk_file_exists && !source.baseline_exists) {
				error = std::format("BlackTek zone Save As destination has a conflicting file: {}", pathText(disk_path));
				return std::nullopt;
			}
			if (disk_file_exists) {
				std::ifstream current(disk_path, std::ios::binary);
				const std::string current_text((std::istreambuf_iterator<char>(current)), std::istreambuf_iterator<char>());
				if (!current || current_text != source.original_text) {
					error = std::format("BlackTek zone Save As destination has conflicting content: {}", pathText(disk_path));
					return std::nullopt;
				}
			}
		}

		const bool semantic_change = formatTable(source.root) != formatTable(source.baseline_root);
		if (semantic_change || source.dirty) {
			output_files.emplace(key, source.root);
		} else if (saving_as && source.baseline_exists) {
			const auto output = transaction.staged_directory_ / source.relative_path;
			std::filesystem::create_directories(output.parent_path(), ec);
			if (ec) {
				error = std::format("Cannot create staged BlackTek zone subdirectory: {}", ec.message());
				return std::nullopt;
			}
			std::ofstream stream(output, std::ios::binary | std::ios::trunc);
			if (!stream) {
				error = std::format("Cannot preserve unchanged BlackTek zone file: {}", pathText(output));
				return std::nullopt;
			}
			stream.write(source.original_text.data(), static_cast<std::streamsize>(source.original_text.size()));
			if (!stream) {
				error = std::format("Cannot copy unchanged BlackTek zone file: {}", pathText(output));
				return std::nullopt;
			}
		}
	}

	for (auto& [key, table] : output_files) {
		for (const auto& record : records_) {
			if (caseFoldedPathKey(record.source_file) == caseFoldedPathKey(std::filesystem::path(key))) {
				std::string validation_error;
				if (!validateProperties(record.properties, validation_error)) {
					error = std::format("BlackTek zone '{}' is invalid: {}", record.name, validation_error);
					return std::nullopt;
				}
			}
		}
		if (table.empty()) {
			std::filesystem::remove(transaction.staged_directory_ / std::filesystem::path(key), ec);
			if (ec && ec != std::errc::no_such_file_or_directory) {
				error = std::format("Cannot remove an empty BlackTek zone file from the staged folder: {}", ec.message());
				return std::nullopt;
			}
			continue;
		}
		const auto relative = std::filesystem::path(key);
		const auto output = transaction.staged_directory_ / relative;
		if (!isWithin(transaction.staged_directory_, output)) {
			error = "BlackTek zone destination path escapes its staged folder";
			return std::nullopt;
		}
		std::filesystem::create_directories(output.parent_path(), ec);
		if (ec) {
			error = std::format("Cannot create staged BlackTek zone subdirectory: {}", ec.message());
			return std::nullopt;
		}
		std::filesystem::remove(output, ec);
		ec.clear();
		if (!writeTable(output, table, error)) return std::nullopt;
	}
	return transaction;
}

bool BlackTekZoneDocument::save(const std::filesystem::path& directory, std::string& error) const {
	if (!enabled_) return true;
	auto transaction = stageSave(directory, error);
	if (!transaction) return false;
	if (!transaction->commit(error)) return false;
	transaction->finalize();
	return true;
}
