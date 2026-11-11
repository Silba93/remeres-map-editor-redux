#ifndef RME_UI_DIALOGS_BLACKTEK_ZONES_WINDOW_H_
#define RME_UI_DIALOGS_BLACKTEK_ZONES_WINDOW_H_

#include "game/blacktek_zones.h"

#include <wx/dialog.h>

class wxCheckBox;
class wxButton;
class wxChoice;
class wxListBox;
class wxStaticText;
class wxTextCtrl;
class Editor;

class BlackTekZonesWindow final : public wxDialog {
public:
	BlackTekZonesWindow(wxWindow* parent, const BlackTekZoneDocument& document, const Position& initial_position);
	static void ShowForEditor(wxWindow* parent, Editor& editor);

	[[nodiscard]] const BlackTekZoneDocument& GetDocument() const noexcept { return document_; }
	[[nodiscard]] std::string GetSelectedZoneName() const;

private:
	void BuildLayout();
	void RefreshZoneList();
	void LoadSelection();
	void UpdateTypedControls(const toml::table& properties);
	bool CommitSelection();
	void OnSelectZone(wxCommandEvent& event);
	void OnPropertiesText(wxCommandEvent& event);
	void OnTypedControlChanged(wxCommandEvent& event);
	void OnAddZone(wxCommandEvent& event);
	void OnRemoveZone(wxCommandEvent& event);
	void OnDuplicateZone(wxCommandEvent& event);
	void OnImportZones(wxCommandEvent& event);
	void OnCopyZone(wxCommandEvent& event);
	void OnPasteZone(wxCommandEvent& event);
	void OnEnabled(wxCommandEvent& event);
	void OnAccept(wxCommandEvent& event);
	void SetValidationMessage(const wxString& message, bool error);

	BlackTekZoneDocument document_;
	Position initial_position_;
	wxButton* enable_ = nullptr;
	wxListBox* zones_ = nullptr;
	wxTextCtrl* name_ = nullptr;
	wxChoice* spawn_type_ = nullptr;
	wxChoice* policy_ = nullptr;
	wxTextCtrl* properties_ = nullptr;
	wxStaticText* validation_ = nullptr;
	std::vector<wxCheckBox*> flags_;
	int loaded_selection_ = wxNOT_FOUND;
	bool loading_selection_ = false;
};

#endif // RME_UI_DIALOGS_BLACKTEK_ZONES_WINDOW_H_
