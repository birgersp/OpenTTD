/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file rail_blueprint.cpp Tests for capturing and positioning rail blueprints. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"
#include "../bridge_map.h"
#include "../rail.h"
#include "../rail_blueprint.h"
#include "../rail_map.h"

#include "../safeguards.h"

TEST_CASE("Rail blueprint preserves signals and sparse offsets")
{
	Map::Allocate(64, 64);
	ResetRailTypes();
	const RailType railtype = GetRailTypeByLabel(RAILTYPE_LABEL_RAIL);
	REQUIRE(railtype != INVALID_RAILTYPE);
	const TileIndex track = TileXY(10, 12);
	const TileIndex depot = TileXY(14, 10);
	MakeRailNormal(track, OWNER_NONE, TrackBits{Track::X}, railtype);
	SetHasSignals(track, true);
	const uint signal = SignalAlongTrackdir(Trackdir::X_NE);
	SetPresentSignals(track, signal);
	SetSignalType(track, Track::X, SignalType::PathOneWay);
	SetSignalVariant(track, Track::X, SignalVariant::Semaphore);
	MakeRailDepot(depot, OWNER_NONE, DepotID::Invalid(), DiagDirection::NW, railtype);

	const auto blueprint = RailBlueprint::Capture({track, TileXY(12, 12), depot});
	REQUIRE(blueprint.pieces.size() == 2);
	auto piece = std::ranges::find(blueprint.pieces, RailBlueprintKind::Track, &RailBlueprintPiece::kind);
	REQUIRE(piece != blueprint.pieces.end());
	CHECK(piece->start == Point(0, 2));
	CHECK(piece->layout == TrackBits{Track::X}.base());
	CHECK(GB(piece->signals, 0, 4) == signal);
	CHECK(GB(piece->signals, 4, 3) == to_underlying(SignalType::PathOneWay));
	CHECK(GB(piece->signals, 7, 1) == to_underlying(SignalVariant::Semaphore));

	const std::set<TileIndex> expected{TileXY(20, 22), TileXY(24, 20)};
	CHECK(std::ranges::equal(blueprint.Preview(TileXY(20, 20)), expected, {}, &RailBlueprintPreview::value_type::first));
	const Point cursor{22 * TILE_SIZE + TILE_SIZE / 2, 21 * TILE_SIZE + TILE_SIZE / 2};
	CHECK(blueprint.PlacementOrigin(cursor) == TileXY(20, 20));
	CHECK(std::ranges::equal(blueprint.Preview(blueprint.PlacementOrigin(cursor), false), expected, {}, &RailBlueprintPreview::value_type::first));
}

TEST_CASE("Rail blueprint includes both ramps and the deck of a selected bridge")
{
	Map::Allocate(64, 64);
	ResetRailTypes();
	const RailType railtype = GetRailTypeByLabel(RAILTYPE_LABEL_RAIL);
	REQUIRE(railtype != INVALID_RAILTYPE);
	const TileIndex north = TileXY(10, 10);
	const TileIndex south = TileXY(13, 10);
	MakeRailBridgeRamp(north, OWNER_NONE, 0, DiagDirection::SW, railtype);
	MakeRailBridgeRamp(south, OWNER_NONE, 0, DiagDirection::NE, railtype);

	for (const auto &selection : {std::set{south}, std::set{north, south}}) {
		const auto blueprint = RailBlueprint::Capture(selection);
		REQUIRE(blueprint.pieces.size() == 1);
		CHECK(blueprint.pieces.front().kind == RailBlueprintKind::Bridge);
		const std::set<TileIndex> expected{TileXY(25, 20), TileXY(26, 20), TileXY(27, 20), TileXY(28, 20)};
		CHECK(std::ranges::equal(blueprint.Preview(TileXY(25, 20)), expected, {}, &RailBlueprintPreview::value_type::first));
		const Point centre{27 * TILE_SIZE, 20 * TILE_SIZE + TILE_SIZE / 2};
		CHECK(blueprint.PlacementOrigin(centre) == TileXY(25, 20));
		CHECK(blueprint.PlacementOrigin({centre.x - static_cast<int>(TILE_SIZE / 2), centre.y}) == TileXY(25, 20));
		CHECK(blueprint.PlacementOrigin({centre.x - static_cast<int>(TILE_SIZE / 2) - 1, centre.y}) == TileXY(24, 20));
		CHECK(blueprint.PlacementOrigin({TILE_SIZE, centre.y}) == INVALID_TILE);
		CHECK(blueprint.PlacementOrigin({62 * TILE_SIZE, centre.y}) == INVALID_TILE);
	}
}

TEST_CASE("Rail blueprint rotation turns track geometry and signal directions together")
{
	Map::Allocate(64, 64);
	static constexpr TrackIndexArray<Track> rotated_tracks{Track::Y, Track::X, Track::Right, Track::Left, Track::Upper, Track::Lower};
	for (Track track : TRACK_BIT_ALL) {
		for (Trackdir direction : TrackToTrackdirBits(track)) {
			RailBlueprint blueprint;
			const uint8_t signal_style = 0xD0; // One-way path semaphore.
			const uint64_t signal = static_cast<uint64_t>(signal_style | SignalAlongTrackdir(direction)) << (8 * to_underlying(track));
			blueprint.pieces.push_back({RailBlueprintKind::Track, {0, 1}, {0, 1}, RAILTYPE_LABEL_RAIL, TrackBits{track}.base(), signal});
			blueprint.pieces.push_back({RailBlueprintKind::Depot, {2, 0}, {2, 0}, RAILTYPE_LABEL_RAIL, to_underlying(DiagDirection::NE)});

			blueprint.RotateClockwise();

			const auto preview = blueprint.Preview(TileXY(20, 20));
			const auto outline = blueprint.Preview(TileXY(20, 20), false);
			REQUIRE(preview.size() == 2);
			REQUIRE(outline.size() == 2);
			const auto &rotated = preview.at(TileXY(21, 22));
			const auto rotated_track = rotated_tracks[track];
			CHECK(rotated.tracks == TrackBits{rotated_track});
			const auto exit = ChangeDiagDir(TrackdirToExitdir(direction), DiagDirDiff::Right90);
			const auto rotated_direction = TrackExitdirToTrackdir(rotated_track, exit);
			CHECK(GB(rotated.signals, 8 * to_underlying(rotated_track), 8) == (signal_style | SignalAlongTrackdir(rotated_direction)));
			CHECK(outline.at(TileXY(21, 22)).tracks.None());
			CHECK(preview.at(TileXY(20, 20)).marker == RailBlueprintKind::Depot);
			CHECK(blueprint.pieces[1].layout == to_underlying(DiagDirection::SE));

			for (int turn = 0; turn < 3; ++turn) blueprint.RotateClockwise();
			CHECK(blueprint.pieces[0].start == Point(0, 1));
			CHECK(blueprint.pieces[0].layout == TrackBits{track}.base());
			CHECK(blueprint.pieces[0].signals == signal);
			CHECK(blueprint.pieces[1].start == Point(2, 0));
		}
	}
}

TEST_CASE("Rail blueprint rotation turns station areas and tunnel spans")
{
	Map::Allocate(64, 64);
	RailBlueprint blueprint;
	blueprint.pieces.push_back({RailBlueprintKind::Station, {0, 0}, {3, 1}, RAILTYPE_LABEL_RAIL, to_underlying(Axis::X)});
	blueprint.pieces.push_back({RailBlueprintKind::Tunnel, {4, 0}, {7, 0}, RAILTYPE_LABEL_RAIL});

	blueprint.RotateClockwise();

	CHECK(blueprint.pieces[0].start == Point(0, 4));
	CHECK(blueprint.pieces[0].end == Point(1, 7));
	CHECK(blueprint.pieces[0].layout == to_underlying(Axis::Y));
	CHECK(blueprint.pieces[1].start == Point(0, 0));
	CHECK(blueprint.pieces[1].end == Point(0, 3));
	const auto preview = blueprint.Preview(TileXY(20, 20));
	CHECK(preview.size() == 10);
	CHECK(preview.at(TileXY(20, 20)).marker == RailBlueprintKind::Tunnel);
	CHECK(preview.at(TileXY(20, 24)).tracks == TrackBits{Track::Y});
}
