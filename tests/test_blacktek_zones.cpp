#include "game/blacktek_zones.h"

#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, std::string_view message) {
	if (!condition) {
		throw std::runtime_error(std::string(message));
	}
}

std::string readFile(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}

void writeFile(const std::filesystem::path& path, std::string_view contents) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
	require(output.good(), "failed to write test fixture");
}

struct TemporaryDirectory {
	std::filesystem::path path;

	TemporaryDirectory() {
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		path = std::filesystem::temp_directory_path() / ("rme-blacktek-zones-" + std::to_string(nonce));
		std::filesystem::create_directories(path);
	}

	~TemporaryDirectory() {
		std::error_code ignored;
		std::filesystem::remove_all(path, ignored);
	}
};

std::filesystem::path fixturePath() {
	return std::filesystem::path("tests") / "fixtures" / "blacktek_zones" / "all_server_features.toml";
}

void copyFixture(const std::filesystem::path& directory) {
	std::filesystem::create_directories(directory);
	std::filesystem::copy_file(fixturePath(), directory / "floor-7.toml");
}

void testVocabularyMatchesTheServerContract() {
	const auto flags = BlackTekZoneDocument::flagNames();
	require(flags.size() == 18, "BlackTek zone flag count changed");
	require(flags.front() == "protection" && flags.back() == "no_npcs", "BlackTek flag vocabulary boundary changed");

	const auto types = BlackTekZoneDocument::spawnTypes();
	require(types.size() == 4, "BlackTek spawn type count changed");
	require(types.front() == "monster" && types.back() == "item", "BlackTek spawn type vocabulary changed");

	const auto policies = BlackTekZoneDocument::policies();
	require(policies.size() == 4, "BlackTek policy count changed");
	require(policies.front() == "fixed" && policies.back() == "triggered", "BlackTek policy vocabulary changed");

	const auto triggers = BlackTekZoneDocument::triggerNames();
	require(triggers.size() == 23, "BlackTek TOML trigger vocabulary count changed");
	require(triggers.front() == "Spawn" && triggers.back() == "LeaveBattle", "BlackTek trigger vocabulary changed");
}

void testFixtureCoversGeometrySpawnFamiliesAndUnknownFields() {
	BlackTekZoneDocument document;
	require(document.load(fixturePath()), "server feature fixture did not load");
	require(document.canSave(), "valid server feature fixture was marked unsafe to save");
	require(document.isEnabled(), "existing zone file did not enable BlackTek support");
	require(document.records().size() == 13, "server feature fixture table count changed");

	const auto* flags = document.find("FlagsOnly");
	require(flags && flags->properties.contains("flags"), "flag-only zone was not retained");
	const auto* boss = document.find("FixedBoss");
	require(boss && boss->properties.contains("master") && boss->properties.contains("minions"), "boss composition was not retained");
	const auto* future = document.find("FutureItemSpawn");
	require(future && future->properties.contains("future_server_option"), "unrecognized nested server field was discarded");
	require(future->properties.contains("item_entries"), "item-spawn fields were discarded");

	const auto geometry = document.geometry();
	bool found_range = false;
	bool found_position = false;
	for (const auto& zone : geometry) {
		if (zone.name == "FixedMonster") {
			found_range = zone.range_start && zone.range_end
				&& *zone.range_start == Position(200, 200, 7)
				&& *zone.range_end == Position(204, 204, 7);
			require(zone.fixed_entry_positions.size() == 2, "fixed spawn offsets were not resolved against the range origin");
			require(zone.fixed_entry_positions.front() == Position(201, 201, 7), "fixed spawn marker position changed");
		}
		if (zone.name == "RelativeMonster") {
			found_position = zone.explicit_positions.size() == 1
				&& zone.explicit_positions.front() == Position(300, 300, 7);
			require(zone.fixed_entry_positions.empty(), "relative spawn was rendered as a fixed position");
		}
	}
	require(found_range, "range geometry was not parsed");
	require(found_position, "explicit position geometry was not parsed");
}

void testUnchangedSaveAndTransactionRollbackPreserveExactBytes() {
	TemporaryDirectory temporary;
	const auto source = temporary.path / "source";
	const auto destination = temporary.path / "destination";
	copyFixture(source);
	const std::string original = readFile(source / "floor-7.toml");
	writeFile(source / "notes.txt", "server-owned sidecar\n");

	BlackTekZoneDocument document;
	require(document.load(source), "source folder did not load");
	std::string error;
	require(document.save(destination, error), "unchanged document save failed");
	require(readFile(destination / "floor-7.toml") == original, "unchanged TOML did not remain byte-identical");
	require(readFile(destination / "notes.txt") == "server-owned sidecar\n", "unowned sidecar was not preserved");
	require(readFile(source / "floor-7.toml") == original, "Save As modified the source folder");

	BlackTekZoneDocument saved_document;
	require(saved_document.load(destination), "saved destination did not reload");
	require(saved_document.rename("FlagsOnly", "FlagsRenamed", error), "zone rename failed before transaction test");
	auto transaction = saved_document.stageSave(destination, error);
	require(transaction.has_value(), "could not stage transactional save");
	require(transaction->commit(error), "could not commit staged save");
	require(readFile(destination / "floor-7.toml") != original, "committed rename did not update destination");
	require(transaction->rollback(error), "could not roll back staged save");
	require(readFile(destination / "floor-7.toml") == original, "rollback did not restore original bytes");
}

void testEditedZoneKeepsUnknownFieldsAndDeletionDoesNotResurrect() {
	TemporaryDirectory temporary;
	const auto directory = temporary.path / "zones";
	copyFixture(directory);
	BlackTekZoneDocument document;
	require(document.load(directory), "fixture folder did not load");
	std::string error;
	require(document.rename("FlagsOnly", "RenamedFlags", error), "zone rename failed");
	require(document.save(directory, error), "zone rename save failed");
	BlackTekZoneDocument renamed;
	require(renamed.load(directory), "renamed folder did not reload");
	require(renamed.find("RenamedFlags") != nullptr, "renamed zone name was not persisted");
	require(renamed.find("FlagsOnly") == nullptr, "old zone name was resurrected after rename");

	const auto* future = renamed.find("FutureItemSpawn");
	require(future != nullptr, "future zone missing before unknown-field edit");
	toml::table changed = future->properties;
	changed.insert_or_assign("editor_note", std::string("changed"));
	require(renamed.replace("FutureItemSpawn", std::move(changed), error), "editing zone properties failed");
	require(renamed.save(directory, error), "edited zone save failed");
	BlackTekZoneDocument edited;
	require(edited.load(directory), "edited folder did not reload");
	const auto* edited_future = edited.find("FutureItemSpawn");
	require(edited_future && edited_future->properties.contains("future_server_option"), "unknown TOML field was lost by an edit");
	require(edited_future->properties.contains("item_entries"), "unsupported item spawn payload was lost by an edit");
	require(edited_future->properties.contains("editor_note"), "edited field did not survive save and reload");

	require(edited.remove("FlagsRenamed"), "zone deletion failed");
	require(edited.save(directory, error), "zone deletion save failed");
	BlackTekZoneDocument after_delete;
	require(after_delete.load(directory), "folder did not load after deletion");
	require(after_delete.find("FlagsRenamed") == nullptr, "deleted zone was resurrected from its original file");
}

void testMalformedAndDuplicateDefinitionsCannotBeOverwritten() {
	TemporaryDirectory temporary;
	const auto directory = temporary.path / "invalid";
	std::filesystem::create_directories(directory);
	writeFile(directory / "broken.toml", "[Broken\nnot = \"valid TOML\"\n");
	BlackTekZoneDocument malformed;
	(void)malformed.load(directory);
	require(!malformed.canSave(), "malformed input was allowed to overwrite source data");
	std::string error;
	require(!malformed.stageSave(directory, error).has_value(), "unsafe malformed document staged a destructive save");

	toml::table zone_properties;
	require(BlackTekZoneDocument::parseProperties("positions = [\"1:1:7\"]\n", zone_properties, error), "duplicate test zone properties did not parse");
	BlackTekZoneDocument document;
	require(document.add("Unique", zone_properties, error), "first zone insertion failed");
	require(!document.add("Unique", std::move(zone_properties), error), "duplicate zone name was accepted");
}

void testDisablingDoesNotEraseExistingSourceFiles() {
	TemporaryDirectory temporary;
	const auto directory = temporary.path / "zones";
	copyFixture(directory);
	const std::string original = readFile(directory / "floor-7.toml");
	BlackTekZoneDocument document;
	require(document.load(directory), "source folder did not load");
	document.setEnabled(false);
	require(readFile(directory / "floor-7.toml") == original, "disabling BlackTek support changed source files in memory");
	std::string error;
	require(document.save(directory, error), "saving after disabling BlackTek support failed");
	require(std::filesystem::exists(directory / "floor-7.toml"), "disabling BlackTek support removed its source file");
	BlackTekZoneDocument reloaded;
	require(reloaded.load(directory) && reloaded.find("FlagsOnly"), "saving while disabled erased the existing zone definitions");
}

void testExternalSourceEditsAndNewFilesBlockOverwrites() {
	TemporaryDirectory temporary;
	const auto directory = temporary.path / "zones";
	copyFixture(directory);
	BlackTekZoneDocument document;
	require(document.load(directory), "source folder did not load before external-edit check");
	const auto source_file = directory / "floor-7.toml";
	const std::string original = readFile(source_file);
	const std::string external = original + "\n# external server edit\n";
	writeFile(source_file, external);
	std::string error;
	require(document.rename("RelativeMonster", "RenamedByEditor", error), "editor-side staged rename failed");
	require(!document.stageSave(directory, error).has_value(), "external edit to a known TOML file was overwritten");
	require(readFile(source_file) == external, "conflict detection changed the external edit");

	const auto clean_directory = temporary.path / "new-file-conflict";
	copyFixture(clean_directory);
	BlackTekZoneDocument with_new_file;
	require(with_new_file.load(clean_directory), "source folder did not load before new-file check");
	const auto external_file = clean_directory / "external.toml";
	writeFile(external_file, "[ServerOwned]\npositions = [\"44:55:7\"]\n");
	require(with_new_file.rename("RelativeMonster", "RenamedByEditor", error), "editor-side rename failed before new-file conflict");
	require(!with_new_file.stageSave(clean_directory, error).has_value(), "new external TOML file was silently dropped");
	require(readFile(external_file) == "[ServerOwned]\npositions = [\"44:55:7\"]\n", "new external TOML file changed during conflict detection");
}

void testForeignSaveAsFilesAndZoneNamesBlockOverwrite() {
	TemporaryDirectory temporary;
	const auto source = temporary.path / "source";
	const auto destination = temporary.path / "destination";
	copyFixture(source);
	BlackTekZoneDocument document;
	require(document.load(source), "source folder did not load before foreign Save As check");
	std::filesystem::create_directories(destination);
	const std::string foreign = "[ForeignZone]\npositions = [\"1:2:7\"]\n";
	writeFile(destination / "floor-7.toml", foreign);
	std::string error;
	require(!document.stageSave(destination, error).has_value(), "Save As overwrote a foreign file with the same relative path");
	require(readFile(destination / "floor-7.toml") == foreign, "foreign Save As file changed after rejection");

	const auto name_collision = temporary.path / "name-collision";
	BlackTekZoneDocument source_for_name_collision;
	require(source_for_name_collision.load(source), "source folder did not reload for name-collision check");
	std::filesystem::create_directories(name_collision);
	writeFile(name_collision / "foreign.toml", "[FlagsOnly]\npositions = [\"6:7:8\"]\n");
	require(!source_for_name_collision.stageSave(name_collision, error).has_value(), "Save As merged a duplicate zone name from a foreign TOML file");
	require(readFile(name_collision / "foreign.toml") == "[FlagsOnly]\npositions = [\"6:7:8\"]\n", "foreign duplicate-name file changed after rejection");
}

void testUndoSnapshotsInheritTheLatestDiskBaseline() {
	TemporaryDirectory temporary;
	const auto directory = temporary.path / "zones";
	copyFixture(directory);
	writeFile(directory / "keep.txt", "raw unrelated data\n");
	BlackTekZoneDocument original;
	require(original.load(directory), "source folder did not load before undo-baseline check");
	BlackTekZoneDocument edited = original;
	toml::table properties;
	std::string error;
	require(BlackTekZoneDocument::parseProperties("positions = [\"1400:1400:7\"]\nflags = [\"no_walk\"]\n", properties, error), "added zone properties did not parse");
	require(edited.add("UndoAdded", std::move(properties), error), "adding zone before save failed");
	require(edited.save(directory, error), "saving added zone failed");
	edited.markSaved(directory);

	BlackTekZoneDocument undo_add = original;
	undo_add.inheritSaveBaselineFrom(edited);
	require(undo_add.save(directory, error), "saving undo of added zone failed");
	BlackTekZoneDocument after_undo_add;
	require(after_undo_add.load(directory), "folder did not reload after undo of added zone");
	require(after_undo_add.find("UndoAdded") == nullptr, "undo of add left a persisted zone behind");
	require(readFile(directory / "keep.txt") == "raw unrelated data\n", "undo of add changed an unrelated raw file");

	BlackTekZoneDocument before_edit = after_undo_add;
	BlackTekZoneDocument after_edit = before_edit;
	const auto* old_zone = after_edit.find("FlagsOnly");
	require(old_zone != nullptr, "zone missing before edit-save-undo scenario");
	toml::table changed = old_zone->properties;
	changed.insert_or_assign("editor_note", std::string("saved edit"));
	require(after_edit.replace("FlagsOnly", std::move(changed), error), "editing zone before save failed");
	require(after_edit.save(directory, error), "saving edited zone failed");
	after_edit.markSaved(directory);

	BlackTekZoneDocument undo_edit = before_edit;
	undo_edit.inheritSaveBaselineFrom(after_edit);
	require(undo_edit.save(directory, error), "saving undo of edit failed");
	BlackTekZoneDocument after_undo_edit;
	require(after_undo_edit.load(directory), "folder did not reload after undo of edit");
	const auto* restored_zone = after_undo_edit.find("FlagsOnly");
	require(restored_zone && !restored_zone->properties.contains("editor_note"), "undo of edit did not restore the saved semantic state");
	require(restored_zone->properties.contains("flags"), "undo of edit lost original zone properties");
}

void testCaseDistinctZoneNamesGetDistinctPortableFiles() {
	TemporaryDirectory temporary;
	const auto directory = temporary.path / "zones";
	BlackTekZoneDocument document;
	(void)document.load(directory);
	toml::table properties;
	std::string error;
	require(BlackTekZoneDocument::parseProperties("positions = [\"10:10:7\"]\n", properties, error), "case test properties did not parse");
	require(document.add("Zone", properties, error), "first case-distinct zone insertion failed");
	require(document.add("zone", std::move(properties), error), "second case-distinct zone insertion failed");
	const auto* upper = document.find("Zone");
	const auto* lower = document.find("zone");
	require(upper && lower, "case-distinct zone names were merged in memory");
	auto foldAscii = [](std::string value) {
		for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return value;
	};
	require(foldAscii(upper->source_file.filename().string()) != foldAscii(lower->source_file.filename().string()), "case-distinct zone files collide on Windows filesystems");
	require(document.save(directory, error), "saving case-distinct zones failed");
	BlackTekZoneDocument reloaded;
	require(reloaded.load(directory), "case-distinct zone directory did not reload");
	require(reloaded.find("Zone") && reloaded.find("zone"), "case-distinct zones did not both survive save/reload");
}

void testGeometryEditsRefuseLossyOrMissingCoverage() {
	std::string error;
	toml::table properties;
	require(BlackTekZoneDocument::parseProperties("spawn_type = 'monster'\n", properties, error), "spawn-only fixture did not parse");
	require(!BlackTekZoneDocument::validateProperties(properties, error), "spawn type cannot replace required geometry");
	require(BlackTekZoneDocument::parseProperties("positions = ['10:10:7', 'broken']\nrange = {start='10:10:7', end='12:12:7'}\n", properties, error), "mixed geometry fixture did not parse");
	require(!BlackTekZoneDocument::validateProperties(properties, error), "geometry edits must not silently discard malformed positions");
	require(BlackTekZoneDocument::parseProperties("positions = ['99999999999999999999:10:7']\n", properties, error), "overflow fixture did not parse");
	require(!BlackTekZoneDocument::validateProperties(properties, error), "overflowing coordinates must be rejected");
}

void testUndoMemoryAccountsForNestedPositionData() {
	BlackTekZoneRecord record;
	record.name = "LargeZone";
	toml::array positions;
	for (int index = 0; index < 1000; ++index) positions.push_back("10000:10000:7");
	record.properties.insert_or_assign("positions", std::move(positions));
	require(BlackTekZoneDocument::recordMemorySize(record) >= 12'000,
		"undo memory accounting must include nested position strings");
}

} // namespace

int main() {
	try {
		testVocabularyMatchesTheServerContract();
		testFixtureCoversGeometrySpawnFamiliesAndUnknownFields();
		testUnchangedSaveAndTransactionRollbackPreserveExactBytes();
		testEditedZoneKeepsUnknownFieldsAndDeletionDoesNotResurrect();
		testMalformedAndDuplicateDefinitionsCannotBeOverwritten();
		testDisablingDoesNotEraseExistingSourceFiles();
		testExternalSourceEditsAndNewFilesBlockOverwrites();
		testForeignSaveAsFilesAndZoneNamesBlockOverwrite();
		testUndoSnapshotsInheritTheLatestDiskBaseline();
		testCaseDistinctZoneNamesGetDistinctPortableFiles();
		testGeometryEditsRefuseLossyOrMissingCoverage();
		testUndoMemoryAccountsForNestedPositionData();
		std::cout << "BlackTek zone tests passed.\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << "BlackTek zone test failed: " << error.what() << '\n';
		return 1;
	}
}
