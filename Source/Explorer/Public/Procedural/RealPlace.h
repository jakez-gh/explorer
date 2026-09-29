#pragma once

#include "CoreMinimal.h"
#include "Procedural/WorldGen.h"

/**
 * A real place, built from real data. Council Bluffs, Iowa: streets, buildings, woods, water and
 * elevation baked from OpenStreetMap and a DEM by Tools/fetch_council_bluffs.py into Data/CouncilBluffs.json.
 * It sits at a fixed spot in the otherwise procedural world; WorldGen::Sample and PathsNear defer to it there.
 *
 * World mapping: X = north, Y = east (metres * 100), so the map is not mirrored. Load() must be called on the
 * game thread before any worker thread samples; after that everything here is read-only.
 */
namespace RealPlace
{
	struct FBuilding
	{
		TArray<FVector2D> Outline; // world cm
		FVector2D Centroid = FVector2D::ZeroVector;
		float HeightM = 0.0f;      // 0 = unknown
		FString Type;              // OSM building=*
		FString Name;
		FString Address;
	};

	// World position of the data's origin (Bayliss Park), in world units.
	FVector2D Origin();
	bool Load();
	bool IsLoaded();
	// True if the box (plus margin) touches the place, where mapped streets replace generated ones.
	bool Covers(const FVector2D& Min, const FVector2D& Max, double Margin);

	// True (and S adjusted) inside the place or its blend margin.
	bool Apply(double WorldX, double WorldY, FWorldSample& S);
	void AppendPaths(const FVector2D& Min, const FVector2D& Max, double Margin, TArray<WorldGen::FPath>& Out);
	void BuildingsIn(const FVector2D& Min, const FVector2D& Max, TArray<const FBuilding*>& Out);

	// Nearest mapped road to P: its closest point (world) and width; false if none within MaxDist.
	bool NearestRoad(const FVector2D& P, double MaxDist, FVector2D& OutPoint, float& OutWidth);

	// House lots along residential streets where the map has no building: fills in the neighbourhoods.
	struct FLot { FVector2D Pos; float Yaw; float Width; float Depth; uint32 Seed; bool bFamilyHouse = false; };
	void LotsIn(const FVector2D& Min, const FVector2D& Max, TArray<const FLot*>& Out);

	// Named landmarks (schools, parks...) in world units, for reports and starting positions.
	struct FLandmark { FString Name; FVector2D Pos; };
	const TArray<FLandmark>& Landmarks();

	// World position for local metres east/north of the origin.
	FVector2D FromLocal(double EastM, double NorthM);
	// A place's ground-level eye position for -StartCB style testing.
	bool FindLandmark(const FString& NameContains, FVector2D& OutPos);
}
