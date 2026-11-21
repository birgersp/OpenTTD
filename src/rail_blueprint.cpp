/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file rail_blueprint.cpp Capture, storage and construction of rail blueprints. */

#include "stdafx.h"
#include "rail_blueprint.h"
#include "command_func.h"
#include "company_func.h"
#include "direction_func.h"
#include "error.h"
#include "gfx_func.h"
#include "ini_type.h"
#include "network/network.h"
#include "newgrf_station.h"
#include "rail_cmd.h"
#include "road_map.h"
#include "station_cmd.h"
#include "station_map.h"
#include "strings_func.h"
#include "tilearea_type.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "tunnelbridge.h"
#include "tunnelbridge_cmd.h"
#include "tunnelbridge_map.h"
#include "waypoint_cmd.h"

#include <sstream>

#include "table/strings.h"

#include "safeguards.h"

extern std::string _config_file;

/** Capture a station or waypoint rectangle of matching selected tiles. */
static TileIndex CaptureStationArea(TileIndex tile, const std::set<TileIndex> &selected, std::set<TileIndex> &captured)
{
	const StationID station = GetStationIndex(tile);
	const Axis axis = GetRailStationAxis(tile);
	const auto *spec = GetStationSpec(tile);
	const RailType railtype = GetRailType(tile);
	const bool waypoint = IsRailWaypointTile(tile);
	auto matches = [&](TileIndex other) {
		return selected.contains(other) && !captured.contains(other) && HasStationTileRail(other) &&
				GetStationIndex(other) == station && GetRailStationAxis(other) == axis &&
				GetRailType(other) == railtype && GetStationSpec(other) == spec;
	};

	int width = 1;
	int height = 1;
	if (!waypoint || axis == Axis::Y) {
		while (width < UINT8_MAX && matches(TileAddWrap(tile, width, 0))) ++width;
	}
	if (!waypoint || axis == Axis::X) {
		while (height < UINT8_MAX) {
			int x = 0;
			while (x < width && matches(TileAddWrap(tile, x, height))) ++x;
			if (x != width) break;
			++height;
		}
	}

	TileIndex end = TileAddWrap(tile, width - 1, height - 1);
	for (TileIndex other : TileArea(tile, end)) captured.insert(other);
	return end;
}

RailBlueprint RailBlueprint::Capture(const std::set<TileIndex> &selection)
{
	RailBlueprint blueprint;
	Point origin{static_cast<int>(Map::SizeX()), static_cast<int>(Map::SizeY())};
	std::set<TileIndex> captured;

	for (TileIndex tile : selection) {
		if (captured.contains(tile)) continue;
		RailBlueprintPiece piece;
		TileIndex start = tile;
		TileIndex end = tile;

		if (IsPlainRailTile(tile) || IsLevelCrossingTile(tile)) {
			piece.kind = RailBlueprintKind::Track;
			TrackBits tracks = IsPlainRailTile(tile) ? GetTrackBits(tile) : TrackBits{GetCrossingRailTrack(tile)};
			piece.layout = tracks.base();
			if (IsPlainRailTile(tile)) {
				for (Track track : tracks) {
					if (!HasSignalOnTrack(tile, track)) continue;
					uint signal = (GetPresentSignals(tile) & SignalOnTrack(track)) |
							(to_underlying(GetSignalType(tile, track)) << 4) |
							(to_underlying(GetSignalVariant(tile, track)) << 7);
					piece.signals |= uint64_t{signal} << (8 * to_underlying(track));
				}
			}
		} else if (IsRailDepotTile(tile)) {
			piece.kind = RailBlueprintKind::Depot;
			piece.layout = to_underlying(GetRailDepotDirection(tile));
		} else if (IsTileType(tile, TileType::TunnelBridge) && GetTunnelBridgeTransportType(tile) == TransportType::Rail) {
			end = GetOtherTunnelBridgeEnd(tile);
			captured.insert(end);
			if (end < start) std::swap(start, end);
			piece.kind = IsTunnel(tile) ? RailBlueprintKind::Tunnel : RailBlueprintKind::Bridge;
			if (IsBridge(tile)) piece.layout = GetBridgeType(tile);
		} else if (HasStationTileRail(tile)) {
			piece.kind = IsRailWaypointTile(tile) ? RailBlueprintKind::Waypoint : RailBlueprintKind::Station;
			piece.layout = to_underlying(GetRailStationAxis(tile));
			if (const auto *spec = GetStationSpec(tile); spec != nullptr) {
				piece.grfid = spec->grf_prop.grfid;
				piece.spec = spec->grf_prop.local_id;
			}
			end = CaptureStationArea(tile, selection, captured);
		} else {
			continue;
		}

		piece.start = {static_cast<int>(TileX(start)), static_cast<int>(TileY(start))};
		piece.end = {static_cast<int>(TileX(end)), static_cast<int>(TileY(end))};
		piece.railtype = GetRailTypeInfo(GetRailType(tile))->label;
		origin.x = std::min(origin.x, piece.start.x);
		origin.y = std::min(origin.y, piece.start.y);
		blueprint.pieces.push_back(piece);
	}

	for (auto &piece : blueprint.pieces) {
		piece.start = {piece.start.x - origin.x, piece.start.y - origin.y};
		piece.end = {piece.end.x - origin.x, piece.end.y - origin.y};
	}
	return blueprint;
}

/** Find the inclusive tile bounds of a non-empty blueprint. */
static Rect GetBlueprintBounds(const std::vector<RailBlueprintPiece> &pieces)
{
	Rect bounds{pieces.front().start.x, pieces.front().start.y, pieces.front().end.x, pieces.front().end.y};
	for (const auto &piece : pieces) {
		bounds.left = std::min(bounds.left, piece.start.x);
		bounds.top = std::min(bounds.top, piece.start.y);
		bounds.right = std::max(bounds.right, piece.end.x);
		bounds.bottom = std::max(bounds.bottom, piece.end.y);
	}
	return bounds;
}

/** Turn the blueprint by one clockwise quarter-turn in map coordinates. */
void RailBlueprint::RotateClockwise()
{
	if (this->pieces.empty()) return;
	const Rect bounds = GetBlueprintBounds(this->pieces);
	static constexpr TrackIndexArray<Track> rotated_tracks{Track::Y, Track::X, Track::Right, Track::Left, Track::Upper, Track::Lower};

	for (auto &piece : this->pieces) {
		const Point start{piece.start.y - bounds.top, bounds.right - piece.end.x};
		piece.end = {piece.end.y - bounds.top, bounds.right - piece.start.x};
		piece.start = start;

		switch (piece.kind) {
			case RailBlueprintKind::Track: {
				TrackBits tracks;
				uint64_t signals = 0;
				for (Track track : TrackBits{piece.layout}) {
					const Track rotated_track = rotated_tracks[track];
					tracks.Set(rotated_track);
					const uint8_t signal = GB(piece.signals, 8 * to_underlying(track), 8);
					if (signal == 0) continue;

					uint8_t rotated_signal = signal & 0xF0; // Keep signal type and variant.
					for (Trackdir dir : TrackToTrackdirBits(track)) {
						if ((signal & SignalAlongTrackdir(dir)) == 0) continue;
						const auto exit = ChangeDiagDir(TrackdirToExitdir(dir), DiagDirDiff::Right90);
						rotated_signal |= SignalAlongTrackdir(TrackExitdirToTrackdir(rotated_track, exit));
					}
					signals |= uint64_t{rotated_signal} << (8 * to_underlying(rotated_track));
				}
				piece.layout = tracks.base();
				piece.signals = signals;
				break;
			}

			case RailBlueprintKind::Depot:
				piece.layout = to_underlying(ChangeDiagDir(static_cast<DiagDirection>(piece.layout), DiagDirDiff::Right90));
				break;

			case RailBlueprintKind::Station:
			case RailBlueprintKind::Waypoint:
				piece.layout = to_underlying(OtherAxis(static_cast<Axis>(piece.layout)));
				break;

			default: break;
		}
	}
}

/**
 * Find the tile origin that places the centre of the blueprint closest to the cursor.
 * @param cursor Cursor position in map coordinates, in units of one sixteenth of a tile.
 * @return Placement origin, or INVALID_TILE if the centred footprint is outside the map.
 */
TileIndex RailBlueprint::PlacementOrigin(Point cursor) const
{
	if (cursor.x < 0 || cursor.y < 0 || this->pieces.empty()) return INVALID_TILE;
	const Rect bounds = GetBlueprintBounds(this->pieces);

	/* Rounding to the nearest tile uses half of a tile to account for the centre of each end tile. */
	int x = cursor.x - (bounds.left + bounds.right) * (TILE_SIZE / 2);
	int y = cursor.y - (bounds.top + bounds.bottom) * (TILE_SIZE / 2);
	if (x < 0 || y < 0) return INVALID_TILE;

	int origin_x = x / TILE_SIZE;
	int origin_y = y / TILE_SIZE;
	if (origin_x + bounds.right >= static_cast<int>(Map::MaxX()) || origin_y + bounds.bottom >= static_cast<int>(Map::MaxY())) return INVALID_TILE;

	TileIndex origin = TileXY(origin_x, origin_y);
	return IsValidTile(TileAddWrap(origin, bounds.left, bounds.top)) ? origin : INVALID_TILE;
}

/** Collect the translated footprint and, optionally, its detailed layout. */
RailBlueprintPreview RailBlueprint::Preview(TileIndex origin, bool detailed) const
{
	RailBlueprintPreview preview;
	for (const auto &piece : this->pieces) {
		TileIndex start = TileAddWrap(origin, piece.start.x, piece.start.y);
		TileIndex end = TileAddWrap(origin, piece.end.x, piece.end.y);
		if (start == INVALID_TILE || end == INVALID_TILE) continue;

		const Track span_track = TileX(start) == TileX(end) ? Track::Y : Track::X;
		if (piece.kind == RailBlueprintKind::Tunnel) {
			for (TileIndex tile : {start, end}) {
				auto &data = preview[tile];
				if (!detailed) continue;
				data.tracks.Set(span_track);
				data.marker = piece.kind;
			}
			continue;
		}

		const TileIndex centre = TileXY((TileX(start) + TileX(end)) / 2, (TileY(start) + TileY(end)) / 2);
		for (TileIndex tile : TileArea(start, end)) {
			auto &data = preview[tile];
			if (!detailed) continue;
			switch (piece.kind) {
				case RailBlueprintKind::Track:
					data.tracks.Set(TrackBits{piece.layout});
					data.signals = piece.signals;
					break;

				case RailBlueprintKind::Depot:
					data.tracks.Set(DiagDirToDiagTrack(static_cast<DiagDirection>(piece.layout)));
					data.marker = piece.kind;
					break;

				case RailBlueprintKind::Station:
				case RailBlueprintKind::Waypoint:
					data.tracks.Set(AxisToTrack(static_cast<Axis>(piece.layout)));
					if (tile == centre) data.marker = piece.kind;
					break;

				case RailBlueprintKind::Bridge:
					data.bridge_tracks.Set(span_track);
					if (tile == start || tile == end) data.marker = piece.kind;
					break;

				default: NOT_REACHED();
			}
		}
	}
	return preview;
}

/** Post one construction operation at its translated position. */
static bool PostRailBlueprintPiece(const RailBlueprintPiece &piece, TileIndex origin, CommandCallback *callback)
{
	const auto error = STR_ERROR_RAIL_BLUEPRINT_BUILD;
	TileIndex start = TileAddWrap(origin, piece.start.x, piece.start.y);
	TileIndex end = TileAddWrap(origin, piece.end.x, piece.end.y);
	RailType rt = GetRailTypeByLabel(piece.railtype);
	switch (piece.kind) {
		case RailBlueprintKind::Track:
			return Command<Commands::BuildRailBlueprintTrack>::Post(error, callback, start, rt, TrackBits{piece.layout}, piece.signals);

		case RailBlueprintKind::Depot:
			return Command<Commands::BuildRailDepot>::Post(error, callback, start, rt, static_cast<DiagDirection>(piece.layout));

		case RailBlueprintKind::Bridge:
			return Command<Commands::BuildBridge>::Post(error, callback, end, start, TransportType::Rail, static_cast<BridgeType>(piece.layout), rt, INVALID_ROADTYPE);

		case RailBlueprintKind::Tunnel:
			return Command<Commands::BuildRailBlueprintTunnel>::Post(error, callback, start, end, rt);

		case RailBlueprintKind::Station:
		case RailBlueprintKind::Waypoint: {
			const bool waypoint = piece.kind == RailBlueprintKind::Waypoint;
			auto station_class = waypoint ? STAT_CLASS_WAYP : STAT_CLASS_DFLT;
			uint16_t spec_index = 0;
			if (!piece.grfid.Empty()) {
				const auto *spec = StationClass::GetByGrf(piece.grfid, piece.spec);
				station_class = spec->class_index;
				spec_index = spec->index;
			}
			Axis axis = static_cast<Axis>(piece.layout);
			uint8_t width = piece.end.x - piece.start.x + 1;
			uint8_t height = piece.end.y - piece.start.y + 1;
			if (waypoint) {
				return Command<Commands::BuildRailBlueprintWaypoint>::Post(error, callback, start, rt, axis,
						axis == Axis::X ? height : width, station_class, spec_index);
			}
			return Command<Commands::BuildRailStation>::Post(error, callback, start, rt, axis,
					axis == Axis::X ? height : width, axis == Axis::X ? width : height,
					station_class, spec_index, NEW_STATION, true);
		}

		default: NOT_REACHED();
	}
}

struct RailBlueprintPlacement {
	RailBlueprint blueprint;
	TileIndex origin;
	CompanyID company;
	size_t next_piece = 0;
	bool awaiting_result = false;
};

static std::optional<RailBlueprintPlacement> _rail_blueprint_placement;

void CancelRailBlueprintPlacement()
{
	_rail_blueprint_placement.reset();
}

void CcRailBlueprintPiece(Commands, const CommandCost &result, TileIndex)
{
	if (!_rail_blueprint_placement.has_value()) return;
	if (result.Failed()) {
		CancelRailBlueprintPlacement();
	} else {
		_rail_blueprint_placement->awaiting_result = false;
	}
}

/* Keep at most one blueprint command in flight to stay within the server's command queue. */
static const IntervalTimer<TimerWindow> _rail_blueprint_interval(std::chrono::milliseconds(30), [](auto) {
	if (!_rail_blueprint_placement.has_value()) return;
	auto &placement = *_rail_blueprint_placement;
	if (placement.awaiting_result) return;
	if (!_networking || _local_company != placement.company) {
		CancelRailBlueprintPlacement();
		return;
	}
	if (IsShiftPressed()) return;
	if (placement.next_piece == placement.blueprint.pieces.size()) {
		CcPlaySound_CONSTRUCTION_RAIL(Commands::BuildRailBlueprintTrack, CommandCost{}, placement.origin);
		CancelRailBlueprintPlacement();
		return;
	}

	placement.awaiting_result = true;
	if (!PostRailBlueprintPiece(placement.blueprint.pieces[placement.next_piece++], placement.origin, CcRailBlueprintPiece)) {
		CancelRailBlueprintPlacement();
	}
});

/** Post the original construction commands at their new positions. */
void RailBlueprint::Build(TileIndex origin) const
{
	const auto error = STR_ERROR_RAIL_BLUEPRINT_BUILD;
	auto show_error = [&](StringID detail) {
		ShowErrorMessage(GetEncodedString(error), GetEncodedString(detail), WarningLevel::Info,
				TileX(origin) * TILE_SIZE, TileY(origin) * TILE_SIZE);
	};

	/* Resolve all coordinates and NewGRF rail types before building anything. */
	for (const auto &piece : this->pieces) {
		if (TileAddWrap(origin, piece.start.x, piece.start.y) == INVALID_TILE ||
				TileAddWrap(origin, piece.end.x, piece.end.y) == INVALID_TILE) {
			show_error(STR_ERROR_RAIL_BLUEPRINT_OUTSIDE_MAP);
			return;
		}
		if (!ValParamRailType(GetRailTypeByLabel(piece.railtype)) ||
				(!piece.grfid.Empty() && StationClass::GetByGrf(piece.grfid, piece.spec) == nullptr)) {
			show_error(STR_ERROR_RAIL_BLUEPRINT_UNAVAILABLE);
			return;
		}
	}

	if (_networking && !IsShiftPressed()) {
		if (_rail_blueprint_placement.has_value()) {
			show_error(STR_ERROR_RAIL_BLUEPRINT_BUSY);
		} else {
			_rail_blueprint_placement.emplace(RailBlueprintPlacement{*this, origin, _local_company});
		}
		return;
	}

	for (const auto &piece : this->pieces) {
		if (!PostRailBlueprintPiece(piece, origin, nullptr)) return;
	}
	if (!IsShiftPressed()) CcPlaySound_CONSTRUCTION_RAIL(Commands::BuildRailBlueprintTrack, CommandCost{}, origin);
}

static std::string RailBlueprintFilename()
{
	return _config_file + ".rail-blueprints";
}

/** Check that packed signals belong to the specified tracks and use valid types. */
static bool IsValidBlueprintTrackLayout(TrackBits tracks, uint64_t signals)
{
	if (tracks.None() || (tracks & TRACK_BIT_ALL) != tracks || (signals >> (8 * to_underlying(Track::End))) != 0) return false;
	for (Track track : TRACK_BIT_ALL) {
		const uint signal = GB(signals, 8 * to_underlying(track), 8);
		if (signal == 0) continue;
		const uint directions = GB(signal, 0, 4);
		if (!tracks.Test(track) || directions == 0 || (directions & SignalOnTrack(track)) != directions ||
				GB(signal, 4, 3) >= to_underlying(SignalType::End)) return false;
	}
	return true;
}

/** Validate a piece read from the local blueprint library. */
static bool IsValidBlueprintPiece(const RailBlueprintPiece &piece)
{
	if (piece.start.x < 0 || piece.start.y < 0 || piece.end.x < piece.start.x || piece.end.y < piece.start.y ||
			piece.end.x >= static_cast<int>(MAX_MAP_SIZE) || piece.end.y >= static_cast<int>(MAX_MAP_SIZE)) return false;
	int dx = piece.end.x - piece.start.x;
	int dy = piece.end.y - piece.start.y;

	switch (piece.kind) {
		case RailBlueprintKind::Track:
			return dx == 0 && dy == 0 && piece.layout <= TRACK_BIT_ALL.base() && IsValidBlueprintTrackLayout(TrackBits{piece.layout}, piece.signals);
		case RailBlueprintKind::Depot: return dx == 0 && dy == 0 && IsValidDiagDirection(static_cast<DiagDirection>(piece.layout));
		case RailBlueprintKind::Bridge:
		case RailBlueprintKind::Tunnel: return (dx == 0) != (dy == 0);
		case RailBlueprintKind::Station: return dx < UINT8_MAX && dy < UINT8_MAX && IsValidAxis(static_cast<Axis>(piece.layout));
		case RailBlueprintKind::Waypoint:
			if (!IsValidAxis(static_cast<Axis>(piece.layout))) return false;
			return piece.layout == to_underlying(Axis::X) ? dx == 0 && dy < UINT8_MAX : dy == 0 && dx < UINT8_MAX;
		default: return false;
	}
}

std::vector<RailBlueprint> LoadRailBlueprints()
{
	IniFile ini;
	ini.LoadFromDisk(RailBlueprintFilename(), Subdirectory::None);

	std::vector<RailBlueprint> blueprints;
	for (const auto &group : ini.groups) {
		if (group.name != "rail-blueprint") continue;
		const auto *name = group.GetItem("name");
		if (name == nullptr || !name->value.has_value() || name->value->empty()) continue;

		RailBlueprint blueprint;
		blueprint.name = *name->value;
		for (const auto &item : group.items) {
			if (item.name != "piece" || !item.value.has_value()) continue;
			std::istringstream line(*item.value);
			unsigned kind, layout;
			RailBlueprintPiece piece;
			line >> kind >> piece.start.x >> piece.start.y >> piece.end.x >> piece.end.y >> layout >> piece.signals >> piece.spec;
			for (BaseLabel *label : {static_cast<BaseLabel *>(&piece.railtype), static_cast<BaseLabel *>(&piece.grfid)}) {
				for (auto &byte : *label) {
					unsigned value = 0;
					if (!(line >> value) || value > UINT8_MAX) line.setstate(std::ios::failbit);
					byte = static_cast<uint8_t>(value);
				}
			}
			if (!line || kind >= to_underlying(RailBlueprintKind::End) || layout > UINT8_MAX) {
				blueprint.pieces.clear();
				break;
			}
			piece.kind = static_cast<RailBlueprintKind>(kind);
			piece.layout = static_cast<uint8_t>(layout);
			if (!IsValidBlueprintPiece(piece)) {
				blueprint.pieces.clear();
				break;
			}
			blueprint.pieces.push_back(piece);
		}
		if (!blueprint.pieces.empty()) blueprints.push_back(std::move(blueprint));
	}
	return blueprints;
}

bool SaveRailBlueprints(const std::vector<RailBlueprint> &blueprints)
{
	IniFile ini;
	for (const auto &blueprint : blueprints) {
		auto &group = ini.CreateGroup("rail-blueprint");
		group.CreateItem("name").SetValue(fmt::format("\"{}\"", blueprint.name));
		for (const auto &piece : blueprint.pieces) {
			std::string line = fmt::format("{} {} {} {} {} {} {} {}", to_underlying(piece.kind),
					piece.start.x, piece.start.y, piece.end.x, piece.end.y, piece.layout, piece.signals, piece.spec);
			for (const BaseLabel *label : {static_cast<const BaseLabel *>(&piece.railtype), static_cast<const BaseLabel *>(&piece.grfid)}) {
				for (uint8_t byte : *label) fmt::format_to(std::back_inserter(line), " {}", byte);
			}
			group.CreateItem("piece").SetValue(line);
		}
	}
	return ini.SaveToDisk(RailBlueprintFilename());
}

/**
 * Execute a sequence of dependent construction commands, charging for each completed operation.
 * The outer command uses NoTest because later operations depend on changing the map.
 */
class RailBlueprintConstruction {
	DoCommandFlags flags;
	CommandCost cost{ExpensesType::Construction};
	bool changed = false;

public:
	explicit RailBlueprintConstruction(DoCommandFlags flags) : flags(flags) {}

	template <Commands cmd, typename... Args>
	CommandCost Run(Args... args)
	{
		auto test_flags = DoCommandFlags{this->flags}.Reset(DoCommandFlag::Execute) | CommandFlagsToDCFlags(GetCommandFlags<cmd>());
		auto estimate = Command<cmd>::Do(test_flags, args...);
		if (estimate.Failed()) return estimate;
		if (!this->flags.Test(DoCommandFlag::Execute)) return estimate;
		auto total = this->cost;
		total.AddCost(estimate.GetCost());
		if (!CheckCompanyHasMoney(total)) return total;

		auto result = Command<cmd>::Do(test_flags | DoCommandFlag::Execute, args...);
		if (result.Succeeded()) {
			this->cost.AddCost(result.GetCost());
			this->changed = true;
		}
		return result;
	}

	CommandCost Finish(CommandCost result = {}) const
	{
		return this->changed ? this->cost : result;
	}
};

CommandCost CmdBuildRailBlueprintTrack(DoCommandFlags flags, TileIndex tile, RailType railtype, TrackBits tracks, uint64_t signals)
{
	if (!ValParamRailType(railtype) || !IsValidBlueprintTrackLayout(tracks, signals)) return CMD_ERROR;

	RailBlueprintConstruction build(flags);
	CommandCost estimate(ExpensesType::Construction);
	for (Track track : tracks) {
		auto result = build.Run<Commands::BuildRail>(tile, railtype, track, false);
		if (result.Failed() && (result.GetErrorMessage() != STR_ERROR_ALREADY_BUILT || GetRailType(tile) != railtype)) {
			return build.Finish(std::move(result));
		}
		if (result.Succeeded()) estimate.AddCost(result.GetCost());
	}
	if (!flags.Test(DoCommandFlag::Execute)) return estimate;

	for (Track track : tracks) {
		uint signal = GB(signals, 8 * to_underlying(track), 8);
		if (signal == 0) continue;
		auto result = build.Run<Commands::BuildSignal>(tile, track, static_cast<SignalType>(GB(signal, 4, 3)),
				static_cast<SignalVariant>(GB(signal, 7, 1)), false, false, false,
				SignalType::Block, SignalType::Block, 0, GB(signal, 0, 4));
		if (result.Failed()) return build.Finish(std::move(result));
	}
	return build.Finish();
}

CommandCost CmdBuildRailBlueprintWaypoint(DoCommandFlags flags, TileIndex tile, RailType railtype, Axis axis, uint8_t count, StationClassID station_class, uint16_t station_type)
{
	if (!ValParamRailType(railtype) || !IsValidAxis(axis) || count == 0 || count > _settings_game.station.station_spread) return CMD_ERROR;
	const Axis span_axis = OtherAxis(axis);
	const TileIndex end = TileAddWrap(tile, span_axis == Axis::X ? count - 1 : 0, span_axis == Axis::Y ? count - 1 : 0);
	if (end == INVALID_TILE || station_class.base() >= StationClass::GetClassCount()) return CMD_ERROR;
	const StationClass *cls = StationClass::Get(station_class);
	if (!IsWaypointClass(*cls) || station_type >= cls->GetSpecCount()) return CMD_ERROR;

	RailBlueprintConstruction build(flags);
	CommandCost estimate(ExpensesType::Construction);
	for (TileIndex current : TileArea(tile, end)) {
		auto result = build.Run<Commands::BuildRail>(current, railtype, AxisToTrack(axis), false);
		if (result.Failed() && (result.GetErrorMessage() != STR_ERROR_ALREADY_BUILT || GetRailType(current) != railtype)) {
			return build.Finish(std::move(result));
		}
		if (result.Succeeded()) estimate.AddCost(result.GetCost());
	}
	if (!flags.Test(DoCommandFlag::Execute)) return estimate;

	auto result = build.Run<Commands::BuildRailWaypoint>(tile, axis, span_axis == Axis::X ? count : 1, span_axis == Axis::Y ? count : 1,
			station_class, station_type, NEW_STATION, true);
	return build.Finish(std::move(result));
}

CommandCost CmdBuildRailBlueprintTunnel(DoCommandFlags flags, TileIndex tile, TileIndex exit, RailType railtype)
{
	if (flags.Test(DoCommandFlag::Execute)) return Command<Commands::BuildTunnel>::Do(flags, tile, TransportType::Rail, railtype, INVALID_ROADTYPE);

	auto result = Command<Commands::BuildTunnel>::Do(flags, tile, TransportType::Rail, railtype, INVALID_ROADTYPE);
	if (result.Succeeded() && _build_tunnel_endtile != exit) return CommandCost(STR_ERROR_RAIL_BLUEPRINT_TUNNEL_EXIT);
	return result;
}
