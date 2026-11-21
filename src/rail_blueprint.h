/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file rail_blueprint.h Data and operations for copying rail construction. */

#ifndef RAIL_BLUEPRINT_H
#define RAIL_BLUEPRINT_H

#include "core/geometry_type.hpp"
#include "newgrf_type.h"
#include "rail_type.h"
#include "tile_type.h"
#include "track_type.h"

#include <map>
#include <set>

struct Window;

/** Rail construction captured by a blueprint piece. */
enum class RailBlueprintKind : uint8_t {
	Track,
	Depot,
	Bridge,
	Tunnel,
	Station,
	Waypoint,
	End,
};

/** A rail construction operation at coordinates relative to the blueprint origin. */
struct RailBlueprintPiece {
	RailBlueprintKind kind{};
	Point start{};
	Point end{}; ///< Other portal or opposite corner, or the same tile for single-tile pieces.
	RailTypeLabel railtype{};
	uint8_t layout = 0; ///< Track bits, depot direction, bridge type, or station axis.
	uint64_t signals = 0; ///< One byte per track (direction mask, type and variant).
	GrfID grfid{}; ///< NewGRF of the station or waypoint specification.
	uint16_t spec = 0; ///< Local NewGRF specification ID.
};

/** Track geometry and construction markers shown on a preview tile. */
struct RailBlueprintPreviewTile {
	TrackBits tracks{};
	TrackBits bridge_tracks{};
	uint64_t signals = 0;
	std::optional<RailBlueprintKind> marker;

	bool operator==(const RailBlueprintPreviewTile &) const = default;
};

using RailBlueprintPreview = std::map<TileIndex, RailBlueprintPreviewTile>;

/** A reusable layout in the local rail blueprint library. */
struct RailBlueprint {
	std::string name;
	std::vector<RailBlueprintPiece> pieces;

	static RailBlueprint Capture(const std::set<TileIndex> &selection);
	void RotateClockwise();
	TileIndex PlacementOrigin(Point cursor) const;
	RailBlueprintPreview Preview(TileIndex origin, bool detailed = true) const;
	void Build(TileIndex origin) const;
};

std::vector<RailBlueprint> LoadRailBlueprints();
bool SaveRailBlueprints(const std::vector<RailBlueprint> &blueprints);
void CancelRailBlueprintPlacement();
void SetRailBlueprintPreview(RailBlueprintPreview preview);
void ShowRailBlueprintWindow(Window *parent);

#endif /* RAIL_BLUEPRINT_H */
