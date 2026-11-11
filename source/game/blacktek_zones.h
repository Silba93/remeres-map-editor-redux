//////////////////////////////////////////////////////////////////////
// This file is part of Remere's Map Editor
//////////////////////////////////////////////////////////////////////

#ifndef RME_BLACKTEK_ZONES_H_
#define RME_BLACKTEK_ZONES_H_

#include "map/position.h"

#include <toml++/toml.hpp>

#include <cstddef>
#include <filesystem>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct BlackTekZoneRecord {
	std::string name;
	std::filesystem::path source_file;
	toml::table properties;
	bool dirty = false;
};

struct BlackTekZoneGeometry {
	std::string name;
	std::optional<Position> range_start;
	std::optional<Position> range_end;
	std::optional<Position> spawn_origin;
	std::vector<Position> explicit_positions;
	std::vector<Position> fixed_entry_positions;
};

class BlackTekZoneSaveTransaction {
public:
	BlackTekZoneSaveTransaction() = default;
	BlackTekZoneSaveTransaction(const BlackTekZoneSaveTransaction&) = delete;
	BlackTekZoneSaveTransaction& operator=(const BlackTekZoneSaveTransaction&) = delete;
	BlackTekZoneSaveTransaction(BlackTekZoneSaveTransaction&& other) noexcept;
	BlackTekZoneSaveTransaction& operator=(BlackTekZoneSaveTransaction&& other) noexcept;
	~BlackTekZoneSaveTransaction();

	[[nodiscard]] bool commit(std::string& error);
	[[nodiscard]] bool rollback(std::string& error) noexcept;
	void finalize(bool retain_recovery_backup = false) noexcept;
	[[nodiscard]] bool prepared() const noexcept;

private:
	friend class BlackTekZoneDocument;

	std::filesystem::path target_directory_;
	std::filesystem::path staged_directory_;
	std::filesystem::path backup_directory_;
	bool target_existed_ = false;
	bool recovery_sentinel_ = false;
	bool committed_ = false;
	bool finalized_ = false;
};

class BlackTekZoneDocument {
public:
	BlackTekZoneDocument() = default;
	BlackTekZoneDocument(const BlackTekZoneDocument&) = default;
	BlackTekZoneDocument& operator=(const BlackTekZoneDocument&) = default;
	BlackTekZoneDocument(BlackTekZoneDocument&&) noexcept = default;
	BlackTekZoneDocument& operator=(BlackTekZoneDocument&&) noexcept = default;

	[[nodiscard]] bool load(const std::filesystem::path& directory);
	[[nodiscard]] bool isEnabled() const noexcept { return enabled_; }
	void setEnabled(bool enabled) noexcept;
	[[nodiscard]] bool canSave() const noexcept { return safe_to_save_; }
	[[nodiscard]] uint64_t revision() const noexcept { return revision_; }
	[[nodiscard]] std::span<const BlackTekZoneRecord> records() const noexcept { return records_; }
	[[nodiscard]] std::span<const BlackTekZoneGeometry> geometry() const noexcept { return geometry_; }
	[[nodiscard]] std::span<const std::string> diagnostics() const noexcept { return diagnostics_; }
	[[nodiscard]] size_t estimatedMemorySize() const noexcept;
	[[nodiscard]] static size_t recordMemorySize(const BlackTekZoneRecord& record) noexcept;

	[[nodiscard]] const BlackTekZoneRecord* find(std::string_view name) const noexcept;
	[[nodiscard]] bool add(std::string name, toml::table properties, std::string& error);
	[[nodiscard]] bool add(BlackTekZoneRecord record, std::string& error);
	[[nodiscard]] bool replace(std::string_view name, toml::table properties, std::string& error);
	[[nodiscard]] bool rename(std::string_view name, std::string new_name, std::string& error);
	[[nodiscard]] bool remove(std::string_view name);
	void markDirty() noexcept;
	void markSaved();
	void markSaved(const std::filesystem::path& directory);
	void inheritSaveBaselineFrom(const BlackTekZoneDocument& other);

	[[nodiscard]] std::optional<BlackTekZoneSaveTransaction> stageSave(
		const std::filesystem::path& directory,
		std::string& error,
		bool retain_recovery_backup = false
	) const;
	[[nodiscard]] bool save(const std::filesystem::path& directory, std::string& error) const;

	[[nodiscard]] static std::string formatProperties(const toml::table& properties);
	[[nodiscard]] static bool parseProperties(std::string_view text, toml::table& properties, std::string& error);
	[[nodiscard]] static bool validateProperties(const toml::table& properties, std::string& error);

	[[nodiscard]] static std::span<const std::string_view> flagNames() noexcept;
	[[nodiscard]] static std::span<const std::string_view> spawnTypes() noexcept;
	[[nodiscard]] static std::span<const std::string_view> policies() noexcept;
	[[nodiscard]] static std::span<const std::string_view> triggerNames() noexcept;

private:
	[[nodiscard]] bool addDraft(std::string name, toml::table properties, std::string& error);
	struct SourceFile {
		std::filesystem::path relative_path;
		toml::table root;
		toml::table baseline_root;
		std::string original_text;
		bool baseline_exists = false;
		bool dirty = false;
	};

	void rebuildGeometry();
	void markModified() noexcept;

	std::filesystem::path source_directory_;
	std::vector<SourceFile> source_files_;
	std::vector<BlackTekZoneRecord> records_;
	std::vector<BlackTekZoneGeometry> geometry_;
	std::vector<std::string> diagnostics_;
	bool enabled_ = false;
	bool safe_to_save_ = true;
	uint64_t revision_ = 0;
};

#endif // RME_BLACKTEK_ZONES_H_
