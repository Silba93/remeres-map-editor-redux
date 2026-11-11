#include "app/main.h"

#include "ui/dialogs/blacktek_zones_window.h"
#include "ui/theme.h"
#include "util/common.h"
#include "util/image_manager.h"
#include "brushes/managers/brush_manager.h"
#include "brushes/zone/zone_brush.h"
#include "editor/editor.h"
#include "editor/operations/blacktek_zone_operations.h"
#include "ui/gui.h"
#include "ui/map_tab.h"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dirdlg.h>

namespace {
	constexpr std::array<const char*, 18> ZONE_FLAG_LABELS = {
		"Protection", "No PvP", "PvP", "No logout",
		"No experience", "No trading", "No transactions",
		"No party", "No drop", "No death",
		"No death penalty", "No equipment loss",
		"No summons", "Ghost", "No pathfinding",
		"No walking", "No monsters", "No NPCs",
	};

	bool isKnownFlag(std::string_view value) {
		const auto flagNames = BlackTekZoneDocument::flagNames();
		return std::ranges::find(flagNames, value) != flagNames.end();
	}

	std::string getString(const toml::table& table, std::string_view key) {
		const auto value = table[key].value<std::string>();
		return value.value_or(std::string{});
	}

	wxString capabilityMessage(const toml::table& properties) {
		const std::string spawnType = getString(properties, "spawn_type");
		if (spawnType == "item") {
			return "Item spawn is declared in BlackTek but this server build has no item entry loader or runtime dispatch.";
		}
		if (spawnType == "npc") {
			const std::string policy = getString(properties, "policy");
			if (!policy.empty() && policy != "fixed" && policy != "relative") {
				return "BlackTek currently dispatches NPC spawns only for fixed and relative policies.";
			}
		}
		return wxEmptyString;
	}

	void replaceKnownFlags(toml::table& properties, const std::vector<wxCheckBox*>& controls) {
		toml::array flags;
		if (toml::array* existing = properties["flags"].as_array()) {
			flags = *existing;
			for (auto it = flags.begin(); it != flags.end();) {
				const auto value = it->value<std::string>();
				if (value && isKnownFlag(*value)) {
					it = flags.erase(it);
				} else {
					++it;
				}
			}
		}
		for (size_t index = 0; index < controls.size(); ++index) {
			if (controls[index]->GetValue()) {
				flags.push_back(std::string(BlackTekZoneDocument::flagNames()[index]));
			}
		}
		if (flags.empty()) {
			properties.erase("flags");
		} else {
			properties.insert_or_assign("flags", std::move(flags));
		}
	}

} // namespace

void BlackTekZonesWindow::ShowForEditor(wxWindow* parent, Editor& editor) {
	if (g_gui.GetCurrentEditor() != &editor || editor.live_manager.IsLive()) {
		return;
	}
	MapTab* tab = g_gui.GetCurrentMapTab();
	const Position origin = tab ? tab->GetScreenCenterPosition() : Position {};
	BlackTekZonesWindow dialog(parent, editor.map.blacktek_zones, origin);
	for (size_t index = 0; index < editor.map.blacktek_zones.records().size(); ++index) {
		if (editor.map.blacktek_zones.records()[index].name == editor.map.selected_blacktek_zone) {
			dialog.zones_->SetSelection(static_cast<int>(index));
			dialog.LoadSelection();
			break;
		}
	}
	if (dialog.ShowModal() != wxID_OK) {
		return;
	}
	std::string error;
	if (!BlackTekZoneOperations::apply(editor, dialog.GetDocument(), error)) {
		wxMessageBox(wxstr(error), "BlackTek Zones and Spawns", wxOK | wxICON_ERROR, parent);
		return;
	}
	editor.map.selected_blacktek_zone = dialog.GetSelectedZoneName();
	if (editor.map.blacktek_zones.isEnabled()
		&& editor.map.blacktek_zones.find(editor.map.selected_blacktek_zone)
		&& g_brush_manager.zone_brush) {
		g_brush_manager.zone_brush->setBlackTekMode();
		g_gui.SelectBrush(g_brush_manager.zone_brush);
		g_gui.SetStatusText("BlackTek zone selected. Paint coverage with the zone brush; erase removes coverage.");
	}
	g_gui.RefreshPalettes(&editor.map);
	g_gui.RefreshView();
}

BlackTekZonesWindow::BlackTekZonesWindow(wxWindow* parent, const BlackTekZoneDocument& document, const Position& initial_position) :
	wxDialog(parent, wxID_ANY, "BlackTek Zones", wxDefaultPosition, wxSize(Theme::Grid(280), Theme::Grid(195)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX),
	document_(document),
	initial_position_(initial_position) {
	SetMinSize(wxSize(Theme::Grid(200), Theme::Grid(140)));
	SetBackgroundColour(Theme::Get(Theme::Role::Surface));
	SetFont(Theme::GetFont());
	BuildLayout();
	RefreshZoneList();
	if (!document_.records().empty()) {
		zones_->SetSelection(0);
		LoadSelection();
	}
	CentreOnParent();
}

void BlackTekZonesWindow::BuildLayout() {
	auto* root = new wxBoxSizer(wxVERTICAL);
	enable_ = new wxButton(this, wxID_ANY, document_.isEnabled() ? "BlackTek zone files enabled" : "Enable BlackTek zone files");
	enable_->SetToolTip("BlackTek loads zones from a sibling <mapname>-zones folder containing TOML files. Existing folders are discovered automatically.");
	enable_->Enable(!document_.isEnabled());
	enable_->Bind(wxEVT_BUTTON, &BlackTekZonesWindow::OnEnabled, this);
	root->Add(enable_, wxSizerFlags().Border(wxALL, Theme::Grid(2)));

	auto* body = new wxBoxSizer(wxHORIZONTAL);
	auto* left = new wxBoxSizer(wxVERTICAL);
	zones_ = new wxListBox(this, wxID_ANY);
	zones_->SetMinSize(wxSize(Theme::Grid(52), Theme::Grid(50)));
	zones_->Bind(wxEVT_LISTBOX, &BlackTekZonesWindow::OnSelectZone, this);
	left->Add(zones_, wxSizerFlags(1).Expand().Border(wxBOTTOM, Theme::Grid(2)));
	auto* zoneActions = new wxFlexGridSizer(2, Theme::Grid(1), Theme::Grid(1));
	auto* add = new wxButton(this, wxID_ADD, "Add");
	add->Bind(wxEVT_BUTTON, &BlackTekZonesWindow::OnAddZone, this);
	zoneActions->Add(add, wxSizerFlags(1).Expand());
	auto* remove = new wxButton(this, wxID_REMOVE, "Remove");
	remove->Bind(wxEVT_BUTTON, &BlackTekZonesWindow::OnRemoveZone, this);
	zoneActions->Add(remove, wxSizerFlags(1).Expand());
	auto* duplicate = new wxButton(this, wxID_ANY, "Duplicate");
	duplicate->Bind(wxEVT_BUTTON, &BlackTekZonesWindow::OnDuplicateZone, this);
	zoneActions->Add(duplicate, wxSizerFlags(1).Expand());
	auto* import_button = new wxButton(this, wxID_ANY, "Import folder...");
	import_button->Bind(wxEVT_BUTTON, &BlackTekZonesWindow::OnImportZones, this);
	zoneActions->Add(import_button, wxSizerFlags(1).Expand());
	auto* copy = new wxButton(this, wxID_COPY, "Copy TOML");
	copy->Bind(wxEVT_BUTTON, &BlackTekZonesWindow::OnCopyZone, this);
	zoneActions->Add(copy, wxSizerFlags(1).Expand());
	auto* paste = new wxButton(this, wxID_PASTE, "Paste TOML");
	paste->Bind(wxEVT_BUTTON, &BlackTekZonesWindow::OnPasteZone, this);
	zoneActions->Add(paste, wxSizerFlags(1).Expand());
	left->Add(zoneActions, wxSizerFlags().Expand());
	body->Add(left, wxSizerFlags(0).Expand().Border(wxALL, Theme::Grid(2)));

	auto* right = new wxBoxSizer(wxVERTICAL);
	auto* nameRow = new wxBoxSizer(wxHORIZONTAL);
	nameRow->Add(new wxStaticText(this, wxID_ANY, "Zone name"), wxSizerFlags().CenterVertical().Border(wxRIGHT, Theme::Grid(2)));
	name_ = new wxTextCtrl(this, wxID_ANY);
	nameRow->Add(name_, wxSizerFlags(1).Expand());
	right->Add(nameRow, wxSizerFlags().Expand().Border(wxBOTTOM, Theme::Grid(2)));

	auto* kindRow = new wxFlexGridSizer(2, Theme::Grid(2), Theme::Grid(2));
	kindRow->Add(new wxStaticText(this, wxID_ANY, "Spawn type"), wxSizerFlags().CenterVertical());
	spawn_type_ = new wxChoice(this, wxID_ANY);
	spawn_type_->Append("Flags only");
	for (const std::string_view type : BlackTekZoneDocument::spawnTypes()) {
		spawn_type_->Append(wxstr(type));
	}
	spawn_type_->SetSelection(0);
	spawn_type_->SetToolTip("BlackTek declares item spawns, but this server build has no item entry loader or spawn dispatch.");
	kindRow->Add(spawn_type_, wxSizerFlags(1).Expand());
	kindRow->Add(new wxStaticText(this, wxID_ANY, "Spawn policy"), wxSizerFlags().CenterVertical());
	policy_ = new wxChoice(this, wxID_ANY);
	for (const std::string_view policy : BlackTekZoneDocument::policies()) {
		policy_->Append(wxstr(policy));
	}
	policy_->SetToolTip("NPC spawns currently support fixed and relative policies. Monster and boss spawns support all four.");
	kindRow->Add(policy_, wxSizerFlags(1).Expand());
	right->Add(kindRow, wxSizerFlags().Expand().Border(wxBOTTOM, Theme::Grid(2)));

	auto* flagBox = new wxStaticBoxSizer(wxVERTICAL, this, "Zone flags");
	auto* flagGrid = new wxFlexGridSizer(3, Theme::Grid(1), Theme::Grid(3));
	flagGrid->AddGrowableCol(0, 1);
	flagGrid->AddGrowableCol(1, 1);
	flagGrid->AddGrowableCol(2, 1);
	flags_.reserve(ZONE_FLAG_LABELS.size());
	for (size_t index = 0; index < ZONE_FLAG_LABELS.size(); ++index) {
		auto* control = new wxCheckBox(flagBox->GetStaticBox(), wxID_ANY, ZONE_FLAG_LABELS[index]);
		control->SetToolTip(wxstr(BlackTekZoneDocument::flagNames()[index]));
		flags_.push_back(control);
		flagGrid->Add(control, wxSizerFlags().Expand());
	}
	flagBox->Add(flagGrid, wxSizerFlags().Expand().Border(wxALL, Theme::Grid(2)));
	right->Add(flagBox, wxSizerFlags(0).Expand().Border(wxBOTTOM, Theme::Grid(2)));

	auto* hint = new wxStaticText(this, wxID_ANY,
		"Edit geometry and spawn details in TOML. Entry positions are offsets from the range start. Use the normal Zone brush to paint coverage; delete a zone here to erase coverage. TOML supports positions/range, monsters with weighted lists and waves, NPCs, boss master/minions and delay, schedules, triggers, and other server fields.");
	hint->SetForegroundColour(Theme::Get(Theme::Role::TextSubtle));
	hint->Wrap(Theme::Grid(205));
	right->Add(hint, wxSizerFlags().Expand().Border(wxBOTTOM, Theme::Grid(1)));
	auto* examples = new wxStaticText(this, wxID_ANY,
		"Monster: monsters = [{ position = \"1:1:0\", list = [{ name = \"Rat\", chance = 70 }, { name = \"Bat\", chance = 30 }], interval = 60, wave = 1 }]\n"
		"Boss: master = { name = \"Dragon\", position = \"2:2:0\" }\n"
		"minions = [{ name = \"Dragon Lord\", position = \"3:2:0\", delay = 5 }]\n"
		"NPC: npcs = [{ name = \"Guide\", position = \"1:1:0\", interval = 0 }]  |  staged wave numbers start at 1. Entry positions are relative to range.start.");
	examples->SetForegroundColour(Theme::Get(Theme::Role::TextSubtle));
	examples->Wrap(Theme::Grid(205));
	right->Add(examples, wxSizerFlags().Expand().Border(wxBOTTOM, Theme::Grid(1)));
	properties_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
		wxTE_MULTILINE | wxTE_DONTWRAP | wxTE_RICH2 | wxHSCROLL | wxTE_PROCESS_TAB);
	properties_->SetFont(Theme::GetFont(10));
	properties_->SetBackgroundColour(Theme::Get(Theme::Role::Background));
	properties_->SetForegroundColour(Theme::Get(Theme::Role::Text));
	properties_->Bind(wxEVT_TEXT, &BlackTekZonesWindow::OnPropertiesText, this);
	right->Add(properties_, wxSizerFlags(1).Expand());
	for (wxCheckBox* flag : flags_) {
		flag->Bind(wxEVT_CHECKBOX, &BlackTekZonesWindow::OnTypedControlChanged, this);
	}
	spawn_type_->Bind(wxEVT_CHOICE, &BlackTekZonesWindow::OnTypedControlChanged, this);
	policy_->Bind(wxEVT_CHOICE, &BlackTekZonesWindow::OnTypedControlChanged, this);

	validation_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
	right->Add(validation_, wxSizerFlags().Expand().Border(wxTOP | wxBOTTOM, Theme::Grid(1)));
	body->Add(right, wxSizerFlags(1).Expand().Border(wxALL, Theme::Grid(2)));
	root->Add(body, wxSizerFlags(1).Expand());

	auto* footer = new wxBoxSizer(wxHORIZONTAL);
	auto* ok = new wxButton(this, wxID_OK, "Apply and paint");
	ok->SetBitmap(IMAGE_MANAGER.GetBitmapBundle(ICON_CHECK));
	ok->Bind(wxEVT_BUTTON, &BlackTekZonesWindow::OnAccept, this);
	auto* cancel = new wxButton(this, wxID_CANCEL, "Cancel");
	cancel->SetBitmap(IMAGE_MANAGER.GetBitmapBundle(ICON_XMARK));
	cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
	footer->AddStretchSpacer();
	footer->Add(ok, wxSizerFlags().Border(wxRIGHT, Theme::Grid(1)));
	footer->Add(cancel);
	root->Add(footer, wxSizerFlags().Expand().Border(wxALL, Theme::Grid(2)));
	SetSizer(root);
}

std::string BlackTekZonesWindow::GetSelectedZoneName() const {
	const int selection = zones_->GetSelection();
	if (selection < 0 || selection >= static_cast<int>(document_.records().size())) {
		return {};
	}
	return document_.records()[static_cast<size_t>(selection)].name;
}

void BlackTekZonesWindow::UpdateTypedControls(const toml::table& properties) {
	const std::string spawnType = getString(properties, "spawn_type");
	const wxString typeText = spawnType.empty() ? wxString("Flags only") : wxstr(spawnType);
	if (spawn_type_->FindString(typeText) == wxNOT_FOUND) {
		spawn_type_->Append(typeText);
	}
	spawn_type_->SetStringSelection(typeText);

	const std::string spawnPolicy = getString(properties, "policy");
	if (spawnPolicy.empty()) {
		policy_->SetSelection(wxNOT_FOUND);
	} else {
		const wxString policyText = wxstr(spawnPolicy);
		if (policy_->FindString(policyText) == wxNOT_FOUND) {
			policy_->Append(policyText);
		}
		policy_->SetStringSelection(policyText);
	}

	const toml::array* zoneFlags = properties["flags"].as_array();
	for (size_t index = 0; index < flags_.size(); ++index) {
		const bool selected = zoneFlags && std::ranges::any_of(*zoneFlags, [index](const toml::node& node) {
			return node.value<std::string>().value_or("") == BlackTekZoneDocument::flagNames()[index];
		});
		flags_[index]->SetValue(selected);
	}
}

void BlackTekZonesWindow::RefreshZoneList() {
	const int previous = loaded_selection_;
	zones_->Clear();
	for (const BlackTekZoneRecord& zone : document_.records()) {
		zones_->Append(wxString::FromUTF8(zone.name));
	}
	if (!document_.records().empty()) {
		const int selected = std::clamp(previous, 0, static_cast<int>(document_.records().size()) - 1);
		zones_->SetSelection(selected);
		loaded_selection_ = selected;
	} else {
		loaded_selection_ = wxNOT_FOUND;
		name_->Clear();
		properties_->Clear();
		spawn_type_->SetSelection(0);
		policy_->SetSelection(wxNOT_FOUND);
		for (wxCheckBox* flag : flags_) {
			flag->SetValue(false);
		}
	}
}

void BlackTekZonesWindow::LoadSelection() {
	const int selection = zones_->GetSelection();
	if (selection < 0 || selection >= static_cast<int>(document_.records().size())) {
		return;
	}
	loading_selection_ = true;
	loaded_selection_ = selection;
	const BlackTekZoneRecord& record = document_.records()[static_cast<size_t>(selection)];
	name_->SetValue(wxString::FromUTF8(record.name));
	properties_->SetValue(wxString::FromUTF8(BlackTekZoneDocument::formatProperties(record.properties)));
	UpdateTypedControls(record.properties);
	loading_selection_ = false;
	SetValidationMessage(capabilityMessage(record.properties), false);
}

void BlackTekZonesWindow::OnPropertiesText(wxCommandEvent& event) {
	if (!loading_selection_) {
		toml::table properties;
		std::string error;
		if (BlackTekZoneDocument::parseProperties(std::string(properties_->GetValue().utf8_string()), properties, error)) {
			UpdateTypedControls(properties);
			SetValidationMessage(capabilityMessage(properties), false);
		} else {
			SetValidationMessage(wxstr(error), true);
		}
	}
	event.Skip();
}

void BlackTekZonesWindow::OnTypedControlChanged(wxCommandEvent&) {
	if (loading_selection_) {
		return;
	}
	toml::table properties;
	std::string error;
	if (!BlackTekZoneDocument::parseProperties(std::string(properties_->GetValue().utf8_string()), properties, error)) {
		SetValidationMessage(wxstr(error), true);
		return;
	}
	const wxString spawnType = spawn_type_->GetStringSelection();
	if (spawnType.empty() || spawnType == "Flags only") {
		properties.erase("spawn_type");
	} else {
		properties.insert_or_assign("spawn_type", std::string(spawnType.utf8_string()));
	}
	const wxString policy = policy_->GetStringSelection();
	if (policy.empty()) {
		properties.erase("policy");
	} else {
		properties.insert_or_assign("policy", std::string(policy.utf8_string()));
	}
	replaceKnownFlags(properties, flags_);
	loading_selection_ = true;
	properties_->ChangeValue(wxstr(BlackTekZoneDocument::formatProperties(properties)));
	UpdateTypedControls(properties);
	loading_selection_ = false;
	SetValidationMessage(capabilityMessage(properties), false);
}

bool BlackTekZonesWindow::CommitSelection() {
	if (loaded_selection_ == wxNOT_FOUND) {
		return true;
	}
	const std::string oldName = document_.records()[static_cast<size_t>(loaded_selection_)].name;
	const std::string newName(name_->GetValue().utf8_string());
	if (newName.empty()) {
		SetValidationMessage("Zone name cannot be empty.", true);
		return false;
	}
	toml::table properties;
	std::string error;
	const std::string raw(properties_->GetValue().utf8_string());
	if (!BlackTekZoneDocument::parseProperties(raw, properties, error)) {
		SetValidationMessage(wxString::FromUTF8(error), true);
		return false;
	}
	if (!BlackTekZoneDocument::validateProperties(properties, error)) {
		SetValidationMessage(wxstr(error), true);
		return false;
	}
	const std::string before = BlackTekZoneDocument::formatProperties(document_.records()[static_cast<size_t>(loaded_selection_)].properties);
	const std::string after = BlackTekZoneDocument::formatProperties(properties);
	if (newName == oldName && before == after) {
		return true;
	}
	if (newName != oldName && !document_.rename(oldName, newName, error)) {
		SetValidationMessage(wxString::FromUTF8(error), true);
		return false;
	}
	SetValidationMessage(capabilityMessage(properties), false);
	if (!document_.replace(newName, std::move(properties), error)) {
		SetValidationMessage(wxString::FromUTF8(error), true);
		return false;
	}
	return true;
}

void BlackTekZonesWindow::OnSelectZone(wxCommandEvent&) {
	if (loading_selection_) {
		return;
	}
	const int requested = zones_->GetSelection();
	if (requested == loaded_selection_) {
		return;
	}
	if (!CommitSelection()) {
		zones_->SetSelection(loaded_selection_);
		return;
	}
	LoadSelection();
}

void BlackTekZonesWindow::OnAddZone(wxCommandEvent&) {
	if (!CommitSelection()) {
		return;
	}
	std::string name = "NewZone";
	for (unsigned int suffix = 2; document_.find(name) != nullptr; ++suffix) {
		name = "NewZone" + std::to_string(suffix);
	}
	toml::table properties;
	properties.insert_or_assign("positions", toml::array{std::format("{}:{}:{}", initial_position_.x, initial_position_.y, initial_position_.z)});
	std::string error;
	if (!document_.add(name, std::move(properties), error)) {
		SetValidationMessage(wxString::FromUTF8(error), true);
		return;
	}
	document_.setEnabled(true);
	enable_->SetLabel("BlackTek zone files enabled");
	enable_->Enable(false);
	RefreshZoneList();
	zones_->SetSelection(static_cast<int>(document_.records().size()) - 1);
	LoadSelection();
	SetValidationMessage("A one tile zone starts at the current view. Adjust positions or range in TOML, then paint more coverage with the Zone brush.", false);
}

void BlackTekZonesWindow::OnRemoveZone(wxCommandEvent&) {
	if (loaded_selection_ < 0 || loaded_selection_ >= static_cast<int>(document_.records().size())) {
		return;
	}
	const std::string name = document_.records()[static_cast<size_t>(loaded_selection_)].name;
	document_.remove(name);
	loaded_selection_ = std::min(loaded_selection_, static_cast<int>(document_.records().size()) - 1);
	RefreshZoneList();
	if (loaded_selection_ != wxNOT_FOUND) {
		zones_->SetSelection(loaded_selection_);
		LoadSelection();
	}
}

void BlackTekZonesWindow::OnDuplicateZone(wxCommandEvent&) {
	if (!CommitSelection() || loaded_selection_ < 0 || loaded_selection_ >= static_cast<int>(document_.records().size())) {
		return;
	}
	const BlackTekZoneRecord& selected = document_.records()[static_cast<size_t>(loaded_selection_)];
	const toml::table properties = selected.properties;
	const std::string base = selected.name + "_copy";
	std::string name = base;
	for (unsigned int suffix = 2; document_.find(name) != nullptr; ++suffix) {
		name = std::format("{}_{}", base, suffix);
	}
	std::string error;
	if (!document_.add(name, properties, error)) {
		SetValidationMessage(wxString::FromUTF8(error), true);
		return;
	}
	document_.setEnabled(true);
	enable_->SetLabel("BlackTek zone files enabled");
	enable_->Enable(false);
	RefreshZoneList();
	zones_->SetSelection(static_cast<int>(document_.records().size()) - 1);
	LoadSelection();
}

void BlackTekZonesWindow::OnImportZones(wxCommandEvent&) {
	wxDirDialog picker(this, "Import BlackTek zone TOML folder", wxEmptyString, wxDD_DIR_MUST_EXIST);
	if (picker.ShowModal() != wxID_OK || !CommitSelection()) {
		return;
	}
	BlackTekZoneDocument imported;
	if (!imported.load(std::filesystem::path(picker.GetPath().utf8_string())) || !imported.canSave()) {
		const auto diagnostics = imported.diagnostics();
		SetValidationMessage(diagnostics.empty() ? wxString::FromUTF8("The selected folder could not be read safely.") : wxString::FromUTF8(diagnostics.front()), true);
		return;
	}
	if (imported.records().empty()) {
		SetValidationMessage("The selected folder contains no zones.", false);
		return;
	}
	std::string error;
	BlackTekZoneDocument candidate = document_;
	for (const BlackTekZoneRecord& record : imported.records()) {
		std::string name = record.name;
		if (candidate.find(name) != nullptr) {
			const std::string base = name + "_imported";
			name = base;
			for (unsigned int suffix = 2; candidate.find(name) != nullptr; ++suffix) {
				name = std::format("{}_{}", base, suffix);
			}
		}
		if (!candidate.add(std::move(name), record.properties, error)) {
			SetValidationMessage(wxString::FromUTF8(error), true);
			return;
		}
	}
	candidate.setEnabled(true);
	document_ = std::move(candidate);
	enable_->SetLabel("BlackTek zone files enabled");
	enable_->Enable(false);
	RefreshZoneList();
	LoadSelection();
	SetValidationMessage(wxstr(std::format("Imported {} zone(s). Review the names and geometry before applying.", imported.records().size())), false);
}

void BlackTekZonesWindow::OnCopyZone(wxCommandEvent&) {
	if (!CommitSelection() || loaded_selection_ < 0 || loaded_selection_ >= static_cast<int>(document_.records().size())) {
		return;
	}
	const BlackTekZoneRecord& record = document_.records()[static_cast<size_t>(loaded_selection_)];
	if (!wxTheClipboard->Open()) {
		SetValidationMessage("Could not open the system clipboard.", true);
		return;
	}
	auto clipboardData = std::make_unique<wxTextDataObject>(wxstr(BlackTekZoneDocument::formatProperties(record.properties)));
	const bool copied = wxTheClipboard->SetData(clipboardData.get());
	if (copied) {
		clipboardData.release();
	}
	wxTheClipboard->Close();
	SetValidationMessage(copied ? "Copied the selected zone properties as TOML." : "Could not copy zone properties to the clipboard.", !copied);
}

void BlackTekZonesWindow::OnPasteZone(wxCommandEvent&) {
	if (!wxTheClipboard->Open()) {
		SetValidationMessage("Could not open the system clipboard.", true);
		return;
	}
	if (!wxTheClipboard->IsSupported(wxDF_TEXT)) {
		wxTheClipboard->Close();
		SetValidationMessage("The clipboard does not contain TOML text.", true);
		return;
	}
	wxTextDataObject data;
	const bool received = wxTheClipboard->GetData(data);
	wxTheClipboard->Close();
	if (!received) {
		SetValidationMessage("Could not read zone properties from the clipboard.", true);
		return;
	}
	if (loaded_selection_ == wxNOT_FOUND) {
		SetValidationMessage("Select a zone before pasting TOML properties.", true);
		return;
	}
	toml::table parsed;
	std::string error;
	if (!BlackTekZoneDocument::parseProperties(std::string(data.GetText().utf8_string()), parsed, error)) {
		SetValidationMessage(wxString::FromUTF8(error), true);
		return;
	}
	properties_->SetValue(wxString::FromUTF8(BlackTekZoneDocument::formatProperties(parsed)));
	SetValidationMessage("Pasted TOML into the editor. Apply validates and saves it to the selected zone.", false);
}

void BlackTekZonesWindow::OnEnabled(wxCommandEvent&) {
	document_.setEnabled(true);
	enable_->SetLabel("BlackTek zone files enabled");
	enable_->Enable(false);
}

void BlackTekZonesWindow::OnAccept(wxCommandEvent&) {
	if (!CommitSelection()) {
		return;
	}
	for (const BlackTekZoneRecord& record : document_.records()) {
		std::string error;
		if (!BlackTekZoneDocument::validateProperties(record.properties, error)) {
			SetValidationMessage(wxstr(std::format("Zone '{}': {}", record.name, error)), true);
			return;
		}
	}
	if (!document_.canSave()) {
		const auto diagnostics = document_.diagnostics();
	SetValidationMessage(diagnostics.empty() ? wxString::FromUTF8("The zone document contains unreadable source data.") : wxstr(diagnostics.front()), true);
		return;
	}
	EndModal(wxID_OK);
}

void BlackTekZonesWindow::SetValidationMessage(const wxString& message, bool error) {
	validation_->SetLabel(message);
	validation_->SetForegroundColour(Theme::Get(error ? Theme::Role::Error : Theme::Role::Warning));
	Layout();
}
