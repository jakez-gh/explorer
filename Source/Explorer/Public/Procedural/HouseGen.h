#pragma once

#include "CoreMinimal.h"

/**
 * Procedural houses with real floor plans. A house footprint is split into rooms (binary space
 * partition), rooms get types by size and floor, every dividing wall gets a doorway (so all rooms
 * connect), outside walls get windows and a front door, and a staircase joins the floors. Furniture is
 * placed per room type, against walls and clear of doorways. Everything is a pure function of the seed.
 *
 * Output is a list of pieces in the house's local frame: X along the width, Y along the depth (front
 * door on +Y), Z up from the ground-floor surface. Units are cm.
 */
namespace HouseGen
{
	enum class ESurface : uint8
	{
		ExteriorWall,   // plaster / stucco
		BrickWall,
		TimberWall,
		InteriorWall,   // painted plaster
		PlankFloor,
		TileFloor,
		Stone,          // plinth, steps, chimney
		ClayRoof,
		SlateRoof,
		Wood,           // frames, doors, stairs, trim
		Carpet,         // procedural brown carpet with tiny round spots of tan and dark brown
		Glass,
		Count
	};

	enum class EFurniture : uint8
	{
		Sofa, ArmChair, CoffeeTable, Bookshelf, DiningTable, DiningChair, Stove, KitchenCabinet,
		Cupboard, Bed, Nightstand, Drawers, Mirror, Shelf, Clock, Heater, CeilingLamp,
		Count
	};

	struct FPiece
	{
		bool bFurniture = false;
		ESurface Surface = ESurface::InteriorWall;
		EFurniture Furniture = EFurniture::Sofa;
		// Boxes: centre, full size and rotation. Furniture: floor position (bottom centre) and yaw.
		FVector Center = FVector::ZeroVector;
		FVector Size = FVector(100.0);
		FRotator Rotation = FRotator::ZeroRotator;
		// Optional colour override (linear, multiplies the surface material).
		bool bTinted = false;
		FLinearColor Tint = FLinearColor::White;
	};

	enum class EStyle : uint8
	{
		Plaster,
		Brick,
		Timber,
	};

	struct FHouse
	{
		TArray<FPiece> Pieces;
		float Width = 0.0f;
		float Depth = 0.0f;
		float Height = 0.0f; // ridge height above the ground floor
		// Clear zones (x0, y0, x1, y1, floorZ) in front of every doorway; furniture is never left inside them.
		TArray<FVector4> Doorways;
		TArray<float> DoorwayZ;
	};

	// Seed picks size, style and layout. Footprint is roughly Width x Depth (cm).
	// ForceWidth/ForceDepth (cm) fix the footprint (0 = random); bFlatRoof gives a parapet roof instead of a gable.
	FHouse Generate(uint32 Seed, EStyle Style, bool bTwoStoreys, float ForceWidth = 0.0f, float ForceDepth = 0.0f, bool bFlatRoof = false, int32 ForceFloors = 0);

	// A village church with a nave, pews, altar, and a bell tower over the entrance. Front door on +Y.
	FHouse GenerateChurch(uint32 Seed, EStyle Style);

	// 1719 Avenue E, Council Bluffs: a hand-planned two-storey house with basement, garage and fence, furnished for a family of seven and a cat.
	FHouse GenerateFamilyHouse1719();
}
