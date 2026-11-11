//////////////////////////////////////////////////////////////////////
// This file is part of Remere's Map Editor
//////////////////////////////////////////////////////////////////////

#include "editor/persistence/map_backup_service.h"
#include "editor/editor.h"
#include "map/map.h"
#include "io/iomap_otbm.h"
#include "util/file_system.h"
#include <ctime>
#include <filesystem>
#include <fstream>
#include <spdlog/spdlog.h>

std::string MapBackupService::GenerateTimestampString(bool compact) {
	std::time_t now = std::time(nullptr);
	std::tm tm_now = *std::localtime(&now);
	char time_buf[64];
	if (compact) {
		std::strftime(time_buf, sizeof(time_buf), "%Y%m%d_%H%M%S", &tm_now);
	} else {
		std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d-%H-%M-%S", &tm_now);
	}
	return std::string(time_buf);
}

void MapBackupService::WriteCrashMarker(const std::vector<std::string>& files) {
	std::string marker_path = nstr(FileSystem::GetLocalDataDirectory()) + ".saving.txt";
	std::ofstream f(marker_path.c_str(), std::ios::trunc | std::ios::out);
	for (const auto& file : files) {
		f << file << '\n';
	}
}

void MapBackupService::ClearCrashMarker() {
	std::string marker_path = nstr(FileSystem::GetLocalDataDirectory()) + ".saving.txt";
	std::remove(marker_path.c_str());
}

bool MapBackupService::CreateSnapshot(
	Editor& editor,
	std::string& out_backup_path,
	std::string& out_error,
	const std::string& operation_tag
) {
	const std::string time_str = GenerateTimestampString(true);

	FileName target_file;
	if (editor.map.hasFile()) {
		FileName src(wxstr(editor.map.getFilename()));
		target_file = src;
		target_file.SetName(src.GetName() + "_" + operation_tag + "_" + time_str);
	} else {
		std::string dir = nstr(FileSystem::GetLocalDataDirectory());
		target_file.Assign(wxstr(dir + "untitled_" + operation_tag + "_" + time_str + ".otbm"));
	}

	out_backup_path = nstr(target_file.GetFullPath());

	const std::string original_spawnfile = editor.map.getSpawnFilename();
	const std::string original_monsterfile = editor.map.getMonsterFilename();
	const std::string original_npcfile = editor.map.getNpcFilename();
	const std::string original_housefile = editor.map.getHouseFilename();
	const std::string original_zonefile = editor.map.getZoneFilename();
	const std::string original_waypointfile = editor.map.getWaypointFilename();
	const BlackTekZoneDocument original_blacktek_zones = editor.map.blacktek_zones;
	const bool original_changed = editor.map.hasChanged();
	const bool original_unnamed = editor.map.isUnnamed();

	const std::string snapshot_base = nstr(target_file.GetName());
	const std::filesystem::path snapshot_directory = std::filesystem::path(nstr(target_file.GetPath(wxPATH_GET_SEPARATOR | wxPATH_GET_VOLUME)));
	const std::filesystem::path snapshot_blacktek_zones = snapshot_directory / (snapshot_base + "-zones");
	bool owns_snapshot_blacktek_zones = false;
	if (editor.map.blacktek_zones.isEnabled()) {
		std::error_code ec;
		if (std::filesystem::exists(snapshot_blacktek_zones, ec) || ec) {
			out_error = "Refusing to overwrite existing BlackTek zone snapshot folder: " + snapshot_blacktek_zones.string();
			return false;
		}
		if (editor.map.hasFile()) {
			const std::filesystem::path source_map(editor.map.getFilename());
			const std::filesystem::path source_blacktek_zones = source_map.parent_path() / (source_map.stem().string() + "-zones");
			const auto source_status = std::filesystem::symlink_status(source_blacktek_zones, ec);
			if (ec && ec != std::errc::no_such_file_or_directory) {
				out_error = "Cannot inspect BlackTek zone source folder: " + ec.message();
				return false;
			}
			if (std::filesystem::is_symlink(source_status)) {
				out_error = "Refusing to snapshot a symbolic link as BlackTek zone data: " + source_blacktek_zones.string();
				return false;
			}
			if (std::filesystem::is_directory(source_status)) {
				std::filesystem::copy(source_blacktek_zones, snapshot_blacktek_zones, std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks, ec);
				if (ec) {
					std::error_code cleanup_error;
					std::filesystem::remove_all(snapshot_blacktek_zones, cleanup_error);
					out_error = "Failed to copy BlackTek zone data into the snapshot: " + ec.message();
					return false;
				}
				owns_snapshot_blacktek_zones = true;
			}
		}
	}
	editor.map.setSpawnFilename(snapshot_base + "-spawn.xml");
	editor.map.setMonsterFilename(snapshot_base + "-monster.xml");
	editor.map.setNpcFilename(snapshot_base + "-npc.xml");
	editor.map.setHouseFilename(snapshot_base + "-house.xml");
	editor.map.setZoneFilename(snapshot_base + "-zones.xml");
	editor.map.setWaypointFilename(snapshot_base + "-waypoint.xml");

	IOMapOTBM mapsaver(editor.map.getVersion());
	const bool save_ok = mapsaver.saveMap(editor.map, target_file);

	// Restore original filenames, unnamed state, and preserve dirty state on both success and failure paths
	editor.map.setSpawnFilename(original_spawnfile);
	editor.map.setMonsterFilename(original_monsterfile);
	editor.map.setNpcFilename(original_npcfile);
	editor.map.setHouseFilename(original_housefile);
	editor.map.setZoneFilename(original_zonefile);
	editor.map.setWaypointFilename(original_waypointfile);
	editor.map.blacktek_zones = original_blacktek_zones;
	editor.map.setUnnamed(original_unnamed);
	if (!original_changed && editor.map.hasChanged()) {
		editor.map.clearChanges();
	}

	if (!save_ok) {
		const std::string dir = nstr(target_file.GetPath(wxPATH_GET_SEPARATOR | wxPATH_GET_VOLUME));
		std::remove(nstr(target_file.GetFullPath()).c_str());
		std::remove((dir + snapshot_base + "-spawn.xml").c_str());
		std::remove((dir + snapshot_base + "-monster.xml").c_str());
		std::remove((dir + snapshot_base + "-npc.xml").c_str());
		std::remove((dir + snapshot_base + "-house.xml").c_str());
		std::remove((dir + snapshot_base + "-zones.xml").c_str());
		std::remove((dir + snapshot_base + "-waypoint.xml").c_str());
		if (owns_snapshot_blacktek_zones) {
			std::error_code ec;
			std::filesystem::remove_all(snapshot_blacktek_zones, ec);
		}

		out_error = "Failed to save backup map snapshot to: " + out_backup_path;
		spdlog::error("MapBackupService::CreateSnapshot: {}", out_error);
		return false;
	}

	spdlog::info("MapBackupService::CreateSnapshot: Created map snapshot at {}", out_backup_path);
	return true;
}
