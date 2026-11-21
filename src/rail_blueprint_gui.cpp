/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file rail_blueprint_gui.cpp Rail blueprint library, painting tools and placement preview. */

#include "stdafx.h"
#include "rail_blueprint.h"
#include "dropdown_func.h"
#include "core/geometry_func.hpp"
#include "error.h"
#include "hotkeys.h"
#include "textbuf_gui.h"
#include "strings_func.h"
#include "tilearea_type.h"
#include "tilehighlight_func.h"
#include "viewport_func.h"
#include "window_gui.h"

#include "table/strings.h"

#include "safeguards.h"

enum RailBlueprintWidgets : WidgetID {
	WID_RBP_LIBRARY,
	WID_RBP_DELETE,
	WID_RBP_PLACE,
	WID_RBP_NEW,
	WID_RBP_TOOLS,
	WID_RBP_ROTATE,
	WID_RBP_DETAILS,
	WID_RBP_PAINT,
	WID_RBP_RECTANGLE,
	WID_RBP_ERASE,
	WID_RBP_CLEAR,
	WID_RBP_SUBMIT,
	WID_RBP_HINT,
};

/** Window for saving selected rail construction and placing a saved layout. */
struct RailBlueprintWindow : Window {
	enum class Mode : uint8_t { Library, Select, Place };

	static inline HotkeyList hotkeys{"railblueprint", {
		Hotkey('R', "rotate", WID_RBP_ROTATE),
	}};

	std::vector<RailBlueprint> library = LoadRailBlueprints();
	std::set<TileIndex> selection;
	RailBlueprint draft;
	RailBlueprint placement;
	Mode mode = Mode::Library;
	int selected = this->library.empty() ? -1 : 0;
	bool rectangle = false;
	bool erase = false;
	bool detailed_preview = true;
	TileIndex last_painted = INVALID_TILE;
	Point preview_cursor{-1, -1};
	TileIndex preview_origin = INVALID_TILE;

	RailBlueprintWindow(WindowDesc &desc, Window *parent) : Window(desc)
	{
		this->parent = parent;
		this->InitNested(0);
		this->UpdateButtons();
	}

	void UpdateButtons()
	{
		bool selecting = this->mode == Mode::Select;
		bool placing = this->mode == Mode::Place;
		this->GetWidget<NWidgetStacked>(WID_RBP_TOOLS)->SetDisplayedPlane(placing ? 1 : 0);
		this->SetWidgetDisabledState(WID_RBP_LIBRARY, this->library.empty());
		this->SetWidgetDisabledState(WID_RBP_PLACE, this->selected < 0);
		this->SetWidgetDisabledState(WID_RBP_DELETE, this->selected < 0);
		this->SetWidgetDisabledState(WID_RBP_ROTATE, !placing);
		this->SetWidgetDisabledState(WID_RBP_PAINT, !selecting);
		this->SetWidgetDisabledState(WID_RBP_RECTANGLE, !selecting);
		this->SetWidgetDisabledState(WID_RBP_ERASE, !selecting);
		this->SetWidgetDisabledState(WID_RBP_CLEAR, !selecting || this->selection.empty());
		this->SetWidgetDisabledState(WID_RBP_SUBMIT, !selecting || this->selection.empty());
		this->SetWidgetLoweredState(WID_RBP_NEW, selecting);
		this->SetWidgetLoweredState(WID_RBP_PAINT, selecting && !this->rectangle);
		this->SetWidgetLoweredState(WID_RBP_RECTANGLE, selecting && this->rectangle);
		this->SetWidgetLoweredState(WID_RBP_ERASE, selecting && this->erase);
		this->SetWidgetLoweredState(WID_RBP_PLACE, placing);
		this->SetWidgetLoweredState(WID_RBP_DETAILS, this->detailed_preview);
		this->SetDirty();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_RBP_LIBRARY && this->selected >= 0) {
			return GetString(STR_JUST_RAW_STRING, this->library[this->selected].name);
		}
		if (widget == WID_RBP_HINT) {
			switch (this->mode) {
				case Mode::Select: return GetString(this->erase ? STR_RAIL_BLUEPRINT_ERASE_HINT : STR_RAIL_BLUEPRINT_SELECT_HINT);
				case Mode::Place: return GetString(STR_RAIL_BLUEPRINT_PLACE_HINT);
				default: break;
			}
		}
		return this->Window::GetWidgetString(widget, stringid);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_RBP_HINT) return;
		for (StringID hint : {STR_RAIL_BLUEPRINT_LIBRARY_HINT, STR_RAIL_BLUEPRINT_SELECT_HINT, STR_RAIL_BLUEPRINT_ERASE_HINT, STR_RAIL_BLUEPRINT_PLACE_HINT}) {
			const auto bounds = GetStringBoundingBox(hint);
			size = maxdim(size, {bounds.width + padding.width, bounds.height + padding.height});
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_RBP_LIBRARY: {
				DropDownList list;
				for (int i = 0; i < static_cast<int>(this->library.size()); ++i) {
					list.push_back(MakeDropDownListStringItem(GetString(STR_JUST_RAW_STRING, this->library[i].name), i));
				}
				ShowDropDownList(this, std::move(list), this->selected, widget);
				break;
			}

			case WID_RBP_DELETE:
				ShowQuery(GetEncodedString(STR_RAIL_BLUEPRINT_DELETE_CAPTION),
						GetEncodedString(STR_RAIL_BLUEPRINT_DELETE_CONFIRM, this->library[this->selected].name), this,
						[](Window *parent, bool confirmed) {
							if (confirmed) static_cast<RailBlueprintWindow *>(parent)->DeleteSelectedBlueprint();
						});
				break;

			case WID_RBP_NEW:
				if (this->mode == Mode::Select) {
					ResetObjectToPlace();
				} else {
					SetObjectToPlaceWnd(SPR_CURSOR_CLONE_TRAIN, PAL_NONE, HT_RECT, this);
					this->mode = Mode::Select;
					this->rectangle = false;
					this->erase = false;
				}
				break;

			case WID_RBP_PAINT:
			case WID_RBP_RECTANGLE:
				this->rectangle = widget == WID_RBP_RECTANGLE;
				break;

			case WID_RBP_ERASE:
				this->erase = !this->erase;
				SetSelectionRed(this->erase);
				break;

			case WID_RBP_CLEAR:
				ShowQuery(GetEncodedString(STR_RAIL_BLUEPRINT_CLEAR_CAPTION), GetEncodedString(STR_RAIL_BLUEPRINT_CLEAR_CONFIRM), this,
						[](Window *parent, bool confirmed) {
							auto *window = static_cast<RailBlueprintWindow *>(parent);
							if (!confirmed || window->mode != Mode::Select) return;
							window->selection.clear();
							ClearRailBlueprintSelection();
							window->UpdateButtons();
						});
				break;

			case WID_RBP_SUBMIT:
				this->draft = RailBlueprint::Capture(this->selection);
				if (this->draft.pieces.empty()) {
					ShowErrorMessage(GetEncodedString(STR_ERROR_RAIL_BLUEPRINT_EMPTY), {}, WarningLevel::Info);
				} else {
					ShowQueryString({}, STR_RAIL_BLUEPRINT_NAME, 64, this, CS_ALPHANUMERAL, QueryStringFlag::LengthIsInChars);
				}
				break;

			case WID_RBP_PLACE:
				if (this->mode == Mode::Place) {
					ResetObjectToPlace();
				} else {
					SetObjectToPlaceWnd(SPR_CURSOR_CLONE_TRAIN, PAL_NONE, HT_SPECIAL, this);
					this->mode = Mode::Place;
					this->placement = this->library[this->selected];
				}
				break;

			case WID_RBP_ROTATE:
				this->placement.RotateClockwise();
				this->InvalidatePlacementPreview();
				break;

			case WID_RBP_DETAILS:
				this->detailed_preview = !this->detailed_preview;
				this->InvalidatePlacementPreview();
				break;

			default: return;
		}
		this->UpdateButtons();
	}

	EventState OnHotkey(int hotkey) override
	{
		if (this->mode == Mode::Place && this->FindChildWindowById(WindowClass::ConfirmPopupQuery, ConfirmPopupQueryWindowNumber::Default) != nullptr) {
			return EventState::Handled;
		}
		return this->Window::OnHotkey(hotkey);
	}

	void DeleteSelectedBlueprint()
	{
		auto entry = this->library.begin() + this->selected;
		RailBlueprint removed = std::move(*entry);
		entry = this->library.erase(entry);
		if (!SaveRailBlueprints(this->library)) {
			this->library.insert(entry, std::move(removed));
			ShowErrorMessage(GetEncodedString(STR_ERROR_RAIL_BLUEPRINT_SAVE), {}, WarningLevel::Info);
			return;
		}

		this->selected = std::min(this->selected, static_cast<int>(this->library.size()) - 1);
		if (this->mode == Mode::Place) ResetObjectToPlace();
		this->UpdateButtons();
	}

	void InvalidatePlacementPreview()
	{
		this->preview_cursor = {-1, -1};
		this->preview_origin = INVALID_TILE;
		SetRailBlueprintPreview({});
	}

	void OnDropdownSelect([[maybe_unused]] WidgetID widget, int index, [[maybe_unused]] int click_result) override
	{
		this->selected = index;
		if (this->mode == Mode::Place) {
			this->placement = this->library[this->selected];
			this->InvalidatePlacementPreview();
		}
		this->UpdateButtons();
	}

	void OnQueryTextFinished(std::optional<std::string> name) override
	{
		if (this->mode != Mode::Select || !name.has_value() || name->find_first_not_of(' ') == std::string::npos || this->draft.pieces.empty()) return;

		this->draft.name = std::move(*name);
		this->library.push_back(std::move(this->draft));
		if (!SaveRailBlueprints(this->library)) {
			this->draft = std::move(this->library.back());
			this->library.pop_back();
			ShowErrorMessage(GetEncodedString(STR_ERROR_RAIL_BLUEPRINT_SAVE), {}, WarningLevel::Info);
			return;
		}

		this->selected = static_cast<int>(this->library.size()) - 1;
		ResetObjectToPlace();
	}

	void ApplyBrush(TileIndex tile)
	{
		if (!IsValidTile(tile)) return;
		if (this->erase) {
			if (this->selection.erase(tile) != 0) RemoveRailBlueprintHighlight(tile);
		} else if (this->selection.insert(tile).second) {
			AddRailBlueprintHighlight(tile);
		}
	}

	/** Fill gaps between mouse samples in map coordinates. */
	void PaintTo(TileIndex tile)
	{
		if (!IsValidTile(tile)) {
			this->last_painted = INVALID_TILE;
			return;
		}
		if (this->last_painted == INVALID_TILE) this->last_painted = tile;
		int x = TileX(this->last_painted);
		int y = TileY(this->last_painted);
		int end_x = TileX(tile);
		int end_y = TileY(tile);
		int dx = std::abs(end_x - x);
		int dy = -std::abs(end_y - y);
		int sx = x < end_x ? 1 : -1;
		int sy = y < end_y ? 1 : -1;
		int error = dx + dy;

		for (;;) {
			this->ApplyBrush(TileXY(x, y));
			if (x == end_x && y == end_y) break;
			int step = 2 * error;
			if (step >= dy) {
				error += dy;
				x += sx;
			}
			if (step <= dx) {
				error += dx;
				y += sy;
			}
		}
		this->last_painted = tile;
	}

	void OnPlaceObject(Point pt, TileIndex tile) override
	{
		if (!IsValidTile(tile) || this->FindChildWindowById(WindowClass::ConfirmPopupQuery, ConfirmPopupQueryWindowNumber::Default) != nullptr) return;
		if (this->mode == Mode::Place) {
			TileIndex origin = this->placement.PlacementOrigin(pt);
			if (origin == INVALID_TILE) {
				ShowErrorMessage(GetEncodedString(STR_ERROR_RAIL_BLUEPRINT_BUILD), GetEncodedString(STR_ERROR_RAIL_BLUEPRINT_OUTSIDE_MAP),
						WarningLevel::Info, pt.x, pt.y);
			} else {
				this->placement.Build(origin);
			}
			return;
		}

		if (this->rectangle) {
			VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_RAIL_BLUEPRINT_RECT);
		} else {
			this->last_painted = tile;
			this->ApplyBrush(tile);
			VpStartDragging(DDSP_RAIL_BLUEPRINT_PAINT);
			this->UpdateButtons();
		}
	}

	void OnPlaceDrag(ViewportPlaceMethod method, ViewportDragDropSelectionProcess process, Point pt) override
	{
		if (process == DDSP_RAIL_BLUEPRINT_RECT) {
			VpSelectTilesWithMethod(pt.x, pt.y, method);
		} else if (pt.x != -1) {
			const bool was_empty = this->selection.empty();
			this->PaintTo(TileVirtXY(pt.x, pt.y));
			if (was_empty != this->selection.empty()) this->UpdateButtons();
		} else {
			this->last_painted = INVALID_TILE;
		}
	}

	void OnPlaceMouseUp([[maybe_unused]] ViewportPlaceMethod method, ViewportDragDropSelectionProcess process, Point pt, TileIndex start, TileIndex end) override
	{
		if (process != DDSP_RAIL_BLUEPRINT_RECT || pt.x == -1) return;
		for (TileIndex tile : TileArea(start, end)) this->ApplyBrush(tile);
		this->UpdateButtons();
	}

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		if (this->mode != Mode::Place) return;
		Point cursor = GetTileBelowCursor();
		if (cursor == this->preview_cursor) return;
		this->preview_cursor = cursor;

		TileIndex origin = this->placement.PlacementOrigin(cursor);
		if (origin == this->preview_origin) return;
		this->preview_origin = origin;
		SetRailBlueprintPreview(origin == INVALID_TILE ? RailBlueprintPreview{} : this->placement.Preview(origin, this->detailed_preview));
	}

	void OnPlaceObjectAbort() override
	{
		this->mode = Mode::Library;
		this->selection.clear();
		this->draft = {};
		this->placement = {};
		this->last_painted = INVALID_TILE;
		this->InvalidatePlacementPreview();
		this->CloseChildWindows(WindowClass::QueryString);
		this->CloseChildWindows(WindowClass::ConfirmPopupQuery);
		ClearRailBlueprintSelection();
		this->UpdateButtons();
	}

	void Close(int data = 0) override
	{
		if (_thd.GetCallbackWnd() == this) ResetObjectToPlace();
		this->Window::Close(data);
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_rail_blueprint_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_RAIL_BLUEPRINT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPIP(4, 3, 4), SetPadding(0, 4, 0, 4),
			NWidget(NWID_HORIZONTAL), SetPIP(0, 3, 0),
				NWidget(WWT_DROPDOWN, Colours::DarkGreen, WID_RBP_LIBRARY), SetFill(1, 1), SetStringTip(STR_RAIL_BLUEPRINT_CHOOSE, STR_RAIL_BLUEPRINT_CHOOSE),
				NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_RBP_PLACE), SetFill(0, 1), SetStringTip(STR_RAIL_BLUEPRINT_PLACE, STR_RAIL_BLUEPRINT_PLACE_HINT),
				NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_RBP_DELETE), SetFill(0, 1), SetStringTip(STR_RAIL_BLUEPRINT_DELETE, STR_RAIL_BLUEPRINT_DELETE_TOOLTIP),
				NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_RBP_NEW), SetFill(0, 1), SetStringTip(STR_RAIL_BLUEPRINT_NEW, STR_RAIL_BLUEPRINT_SELECT_HINT),
			EndContainer(),
			NWidget(NWID_SELECTION, Colours::Invalid, WID_RBP_TOOLS),
				NWidget(NWID_HORIZONTAL), SetPIP(0, 3, 0),
					NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_RBP_PAINT), SetFill(1, 1), SetStringTip(STR_RAIL_BLUEPRINT_PAINT, STR_RAIL_BLUEPRINT_PAINT_TOOLTIP),
					NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_RBP_RECTANGLE), SetFill(1, 1), SetStringTip(STR_RAIL_BLUEPRINT_RECTANGLE, STR_RAIL_BLUEPRINT_RECTANGLE_TOOLTIP),
					NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_RBP_ERASE), SetFill(1, 1), SetStringTip(STR_RAIL_BLUEPRINT_ERASE, STR_RAIL_BLUEPRINT_ERASE_TOOLTIP),
					NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_RBP_CLEAR), SetFill(1, 1), SetStringTip(STR_RAIL_BLUEPRINT_CLEAR, STR_RAIL_BLUEPRINT_CLEAR_TOOLTIP),
					NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_RBP_SUBMIT), SetFill(1, 1), SetStringTip(STR_RAIL_BLUEPRINT_SUBMIT, STR_RAIL_BLUEPRINT_NAME),
				EndContainer(),
				NWidget(NWID_HORIZONTAL), SetPIP(0, 3, 0),
					NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_RBP_ROTATE), SetFill(1, 1), SetStringTip(STR_RAIL_BLUEPRINT_ROTATE, STR_RAIL_BLUEPRINT_ROTATE_TOOLTIP),
					NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_RBP_DETAILS), SetFill(1, 1), SetStringTip(STR_RAIL_BLUEPRINT_DETAILS, STR_RAIL_BLUEPRINT_DETAILS_TOOLTIP),
				EndContainer(),
			EndContainer(),
			NWidget(WWT_LABEL, Colours::Invalid, WID_RBP_HINT), SetFill(1, 0), SetStringTip(STR_RAIL_BLUEPRINT_LIBRARY_HINT, STR_NULL),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _rail_blueprint_desc(
	WindowPosition::Automatic, {}, 0, 0,
	WindowClass::BuildRailBlueprint, WindowClass::BuildToolbar,
	WindowDefaultFlag::Construction, _nested_rail_blueprint_widgets, &RailBlueprintWindow::hotkeys
);

void ShowRailBlueprintWindow(Window *parent)
{
	if (BringWindowToFrontById(WindowClass::BuildRailBlueprint, 0) == nullptr) {
		new RailBlueprintWindow(_rail_blueprint_desc, parent);
	}
}
