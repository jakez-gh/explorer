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
	};

	// Seed picks size, style and layout. Footprint is roughly Width x Depth (cm).
	FHouse Generate(uint32 Seed, EStyle Style, bool bTwoStoreys);

	// A village church with a nave, pews, altar, and a bell tower over the entrance. Front door on +Y.
	FHouse GenerateChurch(uint32 Seed, EStyle Style);
}
