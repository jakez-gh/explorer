#include "Procedural/HouseGen.h"
#include "Procedural/WorldGen.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace HouseGen
{
namespace
{
	constexpr float StoreyHeight = 290.0f;
	constexpr float SlabThickness = 20.0f;
	constexpr float OuterWall = 22.0f;
	constexpr float InnerWall = 12.0f;
	constexpr float DoorWidth = 100.0f;
	constexpr float DoorHeight = 215.0f;
	constexpr float WindowWidth = 120.0f;
	constexpr float WindowHeight = 150.0f;
	constexpr float WindowSill = 80.0f;
	constexpr float MinRoom = 260.0f;
	constexpr float StairWidth = 110.0f;
	constexpr float StairLength = 380.0f;

	enum class ERoom : uint8 { Hall, Living, Kitchen, Dining, Bath, Bedroom, Landing };

	struct FRand
	{
		uint32 Seed;
		int32 N = 0;
		float Next() { return WorldGen::HashFloat(static_cast<int32>(Seed), N++, 4242); }
		float Range(float A, float B) { return FMath::Lerp(A, B, Next()); }
	};

	struct FRect
	{
		float X0, Y0, X1, Y1;
		float W() const { return X1 - X0; }
		float D() const { return Y1 - Y0; }
		float Area() const { return W() * D(); }
		FVector2D Mid() const { return FVector2D((X0 + X1) * 0.5f, (Y0 + Y1) * 0.5f); }
	};

	// A wall segment along X (bAlongX) or Y, at a fixed coordinate, with openings.
	struct FOpening
	{
		float At;      // centre along the wall
		float Width;
		float Bottom;
		float Top;
		bool bWindow;
	};

	struct FWall
	{
		bool bAlongX;
		float Fixed;
		float From, To;
		float Thickness;
		bool bExterior;
		TArray<FOpening> Openings;
	};

	struct FBuilder
	{
		FHouse& House;
		ESurface OuterSurface;

		void Box(ESurface Surface, const FVector& Min, const FVector& Max)
		{
			if (Max.X - Min.X < 1.0f || Max.Y - Min.Y < 1.0f || Max.Z - Min.Z < 1.0f)
			{
				return;
			}
			FPiece P;
			P.Surface = Surface;
			P.Center = (Min + Max) * 0.5f;
			P.Size = Max - Min;
			House.Pieces.Add(P);
		}

		void Furniture(EFurniture Type, const FVector& Floor, float Yaw)
		{
			FPiece P;
			P.bFurniture = true;
			P.Furniture = Type;
			P.Center = Floor;
			P.Rotation = FRotator(0.0f, Yaw, 0.0f);
			House.Pieces.Add(P);
		}

		// A wall from Z0 to Z1, cut around its openings. Exterior walls are faced outside with the house
		// style and inside with plaster: two slabs, each half the thickness.
		void Wall(const FWall& W, float Z0, float Z1)
		{
			TArray<FOpening> Openings = W.Openings;
			Openings.Sort([](const FOpening& A, const FOpening& B) { return A.At < B.At; });
			auto Emit = [&](float A, float B, float Bottom, float Top)
			{
				if (B - A < 1.0f || Top - Bottom < 1.0f)
				{
					return;
				}
				if (W.bExterior)
				{
					const float Half = W.Thickness * 0.5f;
					// The outer half faces away from the house centre.
					const float OutSign = W.Fixed >= 0.0f ? 1.0f : -1.0f;
					const float OuterC = W.Fixed + OutSign * Half * 0.5f;
					const float InnerC = W.Fixed - OutSign * Half * 0.5f;
					Slab(OuterSurface, A, B, OuterC, Half, Bottom, Top, W.bAlongX);
					Slab(ESurface::InteriorWall, A, B, InnerC, Half, Bottom, Top, W.bAlongX);
				}
				else
				{
					Slab(ESurface::InteriorWall, A, B, W.Fixed, W.Thickness, Bottom, Top, W.bAlongX);
				}
			};
			float Cursor = W.From;
			for (const FOpening& O : Openings)
			{
				const float A = O.At - O.Width * 0.5f;
				const float B = O.At + O.Width * 0.5f;
				Emit(Cursor, A, Z0, Z1);
				Emit(A, B, Z0, Z0 + O.Bottom);   // below a window
				Emit(A, B, Z0 + O.Top, Z1);       // lintel
				// Frame and glass.
				const float Frame = 8.0f;
				if (O.bWindow)
				{
					Slab(ESurface::Wood, A, B, W.Fixed, W.Thickness + 4.0f, Z0 + O.Bottom - Frame, Z0 + O.Bottom, W.bAlongX);
					Slab(ESurface::Wood, A, B, W.Fixed, W.Thickness + 4.0f, Z0 + O.Top, Z0 + O.Top + Frame, W.bAlongX);
					Slab(ESurface::Wood, A - Frame, A, W.Fixed, W.Thickness + 4.0f, Z0 + O.Bottom, Z0 + O.Top, W.bAlongX);
					Slab(ESurface::Wood, B, B + Frame, W.Fixed, W.Thickness + 4.0f, Z0 + O.Bottom, Z0 + O.Top, W.bAlongX);
					Slab(ESurface::Wood, O.At - 3.0f, O.At + 3.0f, W.Fixed, 6.0f, Z0 + O.Bottom, Z0 + O.Top, W.bAlongX);
					Slab(ESurface::Glass, A, B, W.Fixed, 2.0f, Z0 + O.Bottom, Z0 + O.Top, W.bAlongX);
					if (W.bExterior)
					{
						// Projecting stone sill and shutters flanking the window on the outside face.
						const float OutSign = W.Fixed >= 0.0f ? 1.0f : -1.0f;
						const float Face = W.Fixed + OutSign * (W.Thickness * 0.5f + 5.0f);
						Slab(ESurface::Stone, A - Frame - 6.0f, B + Frame + 6.0f, Face, 12.0f, Z0 + O.Bottom - Frame - 6.0f, Z0 + O.Bottom - Frame, W.bAlongX);
						Slab(ESurface::Wood, A - Frame - 42.0f, A - Frame - 2.0f, Face - OutSign * 2.0f, 5.0f, Z0 + O.Bottom - Frame, Z0 + O.Top + Frame, W.bAlongX);
						Slab(ESurface::Wood, B + Frame + 2.0f, B + Frame + 42.0f, Face - OutSign * 2.0f, 5.0f, Z0 + O.Bottom - Frame, Z0 + O.Top + Frame, W.bAlongX);
					}
				}
				else
				{
					Slab(ESurface::Wood, A - Frame, A, W.Fixed, W.Thickness + 4.0f, Z0, Z0 + O.Top, W.bAlongX);
					Slab(ESurface::Wood, B, B + Frame, W.Fixed, W.Thickness + 4.0f, Z0, Z0 + O.Top, W.bAlongX);
					Slab(ESurface::Wood, A - Frame, B + Frame, W.Fixed, W.Thickness + 4.0f, Z0 + O.Top, Z0 + O.Top + Frame, W.bAlongX);
				}
				Cursor = B;
			}
			Emit(Cursor, W.To, Z0, Z1);
		}

		void Slab(ESurface Surface, float A, float B, float Fixed, float Thickness, float Z0, float Z1, bool bAlongX)
		{
			if (bAlongX)
			{
				Box(Surface, FVector(A, Fixed - Thickness * 0.5f, Z0), FVector(B, Fixed + Thickness * 0.5f, Z1));
			}
			else
			{
				Box(Surface, FVector(Fixed - Thickness * 0.5f, A, Z0), FVector(Fixed + Thickness * 0.5f, B, Z1));
			}
		}
	};

	// Recursive split into rooms; every split becomes an interior wall with one doorway.
	void Split(const FRect& R, FRand& Rand, int32 Depth, TArray<FRect>& Rooms, TArray<FWall>& Walls)
	{
		const bool bCanX = R.W() >= MinRoom * 2.0f;
		const bool bCanY = R.D() >= MinRoom * 2.0f;
		if ((!bCanX && !bCanY) || (Depth >= 2 && Rand.Next() < 0.35f) || Depth >= 4)
		{
			Rooms.Add(R);
			return;
		}
		const bool bAlongX = bCanX && (!bCanY || R.W() >= R.D()); // split the longer side
		const float T = Rand.Range(0.38f, 0.62f);
		FRect A = R, B = R;
		FWall W;
		W.Thickness = InnerWall;
		W.bExterior = false;
		if (bAlongX)
		{
			const float X = FMath::Lerp(R.X0, R.X1, T);
			A.X1 = X; B.X0 = X;
			W.bAlongX = false; W.Fixed = X; W.From = R.Y0; W.To = R.Y1;
		}
		else
		{
			const float Y = FMath::Lerp(R.Y0, R.Y1, T);
			A.Y1 = Y; B.Y0 = Y;
			W.bAlongX = true; W.Fixed = Y; W.From = R.X0; W.To = R.X1;
		}
		const float Span = W.To - W.From;
		W.Openings.Add({ W.From + FMath::Lerp(DoorWidth, Span - DoorWidth, Rand.Next()), DoorWidth, 0.0f, DoorHeight, false });
		Walls.Add(W);
		Split(A, Rand, Depth + 1, Rooms, Walls);
		Split(B, Rand, Depth + 1, Rooms, Walls);
	}

	bool Blocked(const FVector2D& P, float Radius, const TArray<FWall>& Walls)
	{
		// Keep furniture out of doorways.
		for (const FWall& W : Walls)
		{
			for (const FOpening& O : W.Openings)
			{
				if (O.bWindow)
				{
					continue;
				}
				const FVector2D D = W.bAlongX ? FVector2D(O.At, W.Fixed) : FVector2D(W.Fixed, O.At);
				if (FVector2D::Distance(D, P) < Radius + DoorWidth)
				{
					return true;
				}
			}
		}
		return false;
	}

	void Furnish(FBuilder& B, const FRect& Room, ERoom Type, float Z, FRand& Rand, const TArray<FWall>& Walls)
	{
		const float Inset = 45.0f;
		const FVector2D M = Room.Mid();
		// Place along the wall facing inward; Side: 0 -X wall, 1 +X, 2 -Y, 3 +Y.
		auto AlongWall = [&](EFurniture F, int32 Side, float T, float Depth)
		{
			FVector2D P;
			float Yaw = 0.0f;
			switch (Side)
			{
			case 0: P = FVector2D(Room.X0 + Inset + Depth, FMath::Lerp(Room.Y0 + 80.0f, Room.Y1 - 80.0f, T)); Yaw = 0.0f; break;
			case 1: P = FVector2D(Room.X1 - Inset - Depth, FMath::Lerp(Room.Y0 + 80.0f, Room.Y1 - 80.0f, T)); Yaw = 180.0f; break;
			case 2: P = FVector2D(FMath::Lerp(Room.X0 + 80.0f, Room.X1 - 80.0f, T), Room.Y0 + Inset + Depth); Yaw = 90.0f; break;
			default: P = FVector2D(FMath::Lerp(Room.X0 + 80.0f, Room.X1 - 80.0f, T), Room.Y1 - Inset - Depth); Yaw = -90.0f; break;
			}
			if (!Blocked(P, 60.0f, Walls))
			{
				B.Furniture(F, FVector(P.X, P.Y, Z), Yaw);
			}
		};
		const int32 S = FMath::FloorToInt(Rand.Next() * 4.0f) % 4;
		switch (Type)
		{
		case ERoom::Living:
			AlongWall(EFurniture::Sofa, S, 0.5f, 40.0f);
			B.Furniture(EFurniture::CoffeeTable, FVector(M.X, M.Y, Z), 0.0f);
			AlongWall(EFurniture::ArmChair, (S + 2) % 4, 0.3f, 30.0f);
			AlongWall(EFurniture::Bookshelf, (S + 1) % 4, 0.5f, 10.0f);
			AlongWall(EFurniture::Heater, (S + 3) % 4, 0.8f, 20.0f);
			break;
		case ERoom::Kitchen:
			AlongWall(EFurniture::Stove, S, 0.3f, 15.0f);
			AlongWall(EFurniture::KitchenCabinet, S, 0.75f, 10.0f);
			AlongWall(EFurniture::Cupboard, (S + 1) % 4, 0.5f, 10.0f);
			B.Furniture(EFurniture::DiningTable, FVector(M.X, M.Y, Z), 0.0f);
			B.Furniture(EFurniture::DiningChair, FVector(M.X, M.Y - 70.0f, Z), 90.0f);
			B.Furniture(EFurniture::DiningChair, FVector(M.X, M.Y + 70.0f, Z), -90.0f);
			break;
		case ERoom::Dining:
			B.Furniture(EFurniture::DiningTable, FVector(M.X, M.Y, Z), 0.0f);
			for (int32 i = 0; i < 4; ++i)
			{
				const float A = i * 90.0f;
				const FVector2D O = FVector2D(FMath::Cos(FMath::DegreesToRadians(A)), FMath::Sin(FMath::DegreesToRadians(A))) * 85.0f;
				B.Furniture(EFurniture::DiningChair, FVector(M.X + O.X, M.Y + O.Y, Z), A + 180.0f);
			}
			AlongWall(EFurniture::Cupboard, S, 0.5f, 10.0f);
			AlongWall(EFurniture::Clock, (S + 2) % 4, 0.5f, 5.0f);
			break;
		case ERoom::Bedroom:
			AlongWall(EFurniture::Bed, S, 0.5f, 95.0f);
			AlongWall(EFurniture::Nightstand, S, 0.12f, 10.0f);
			AlongWall(EFurniture::Drawers, (S + 1) % 4, 0.5f, 10.0f);
			AlongWall(EFurniture::Mirror, (S + 2) % 4, 0.5f, 5.0f);
			break;
		case ERoom::Bath:
			AlongWall(EFurniture::Mirror, S, 0.5f, 5.0f);
			AlongWall(EFurniture::Shelf, (S + 1) % 4, 0.5f, 5.0f);
			break;
		case ERoom::Hall:
		case ERoom::Landing:
			AlongWall(EFurniture::Shelf, S, 0.5f, 5.0f);
			break;
		}
		// Ceiling lamp in every room.
		B.Furniture(EFurniture::CeilingLamp, FVector(M.X, M.Y, Z + StoreyHeight - SlabThickness - 45.0f), 0.0f);
	}
}

FHouse Generate(uint32 Seed, EStyle Style, bool bTwoStoreys)
{
	FHouse House;
	FRand Rand{ Seed };
	const float W = FMath::GridSnap(Rand.Range(800.0f, 1300.0f), 10.0f);
	const float D = FMath::GridSnap(Rand.Range(650.0f, 1000.0f), 10.0f);
	const int32 Floors = bTwoStoreys ? 2 : 1;
	House.Width = W;
	House.Depth = D;

	FBuilder B{ House, Style == EStyle::Brick ? ESurface::BrickWall : Style == EStyle::Timber ? ESurface::TimberWall : ESurface::ExteriorWall };
	const float HX = W * 0.5f, HY = D * 0.5f;
	const FRect Inside{ -HX + OuterWall * 0.5f, -HY + OuterWall * 0.5f, HX - OuterWall * 0.5f, HY - OuterWall * 0.5f };

	// Stair along the back-left, the same on every floor.
	const FRect Stair{ Inside.X0, Inside.Y0, Inside.X0 + StairLength, Inside.Y0 + StairWidth };

	// Plinth and entrance step.
	B.Box(ESurface::Stone, FVector(-HX - 20.0f, -HY - 20.0f, -60.0f), FVector(HX + 20.0f, HY + 20.0f, 0.0f));
	const float DoorX = Rand.Range(-HX * 0.4f, HX * 0.4f);
	// Porch: a stone deck, two posts and a flat roof over the front door, a step below.
	B.Box(ESurface::Stone, FVector(DoorX - 160.0f, HY + 20.0f, -60.0f), FVector(DoorX + 160.0f, HY + 190.0f, -8.0f));
	B.Box(ESurface::Stone, FVector(DoorX - 120.0f, HY + 190.0f, -60.0f), FVector(DoorX + 120.0f, HY + 240.0f, -34.0f));
	for (const float Sx : { -1.0f, 1.0f })
	{
		B.Box(ESurface::Wood, FVector(DoorX + Sx * 145.0f - 7.0f, HY + 170.0f, -8.0f), FVector(DoorX + Sx * 145.0f + 7.0f, HY + 184.0f, 260.0f));
	}
	B.Box(ESurface::Wood, FVector(DoorX - 165.0f, HY + 20.0f, 260.0f), FVector(DoorX + 165.0f, HY + 195.0f, 276.0f));
	// The front door stands open, swung into the hall.
	B.Box(ESurface::Wood, FVector(DoorX - DoorWidth * 0.5f - 2.0f, HY - OuterWall - DoorWidth, 0.0f), FVector(DoorX - DoorWidth * 0.5f + 2.0f, HY - OuterWall, DoorHeight));

	for (int32 Floor = 0; Floor < Floors; ++Floor)
	{
		const float Z = Floor * StoreyHeight;
		FRand FloorRand{ Seed * 31u + Floor };

		// Rooms and interior walls.
		TArray<FRect> Rooms;
		TArray<FWall> Walls;
		Split(Inside, FloorRand, 0, Rooms, Walls);

		// A wall added by a later split can end exactly where an earlier wall's doorway is, blocking it.
		// Move each doorway to the middle of the widest stretch of its wall that no other wall meets.
		for (FWall& Wl : Walls)
		{
			if (Wl.Openings.Num() == 0)
			{
				continue;
			}
			TArray<float> Meets;
			Meets.Add(Wl.From);
			for (const FWall& V : Walls)
			{
				if (V.bAlongX == Wl.bAlongX || V.Fixed <= Wl.From || V.Fixed >= Wl.To)
				{
					continue;
				}
				if (FMath::IsNearlyEqual(V.From, Wl.Fixed, 2.0f) || FMath::IsNearlyEqual(V.To, Wl.Fixed, 2.0f))
				{
					Meets.Add(V.Fixed);
				}
			}
			Meets.Add(Wl.To);
			Meets.Sort();
			const float Margin = InnerWall * 0.5f + 25.0f;
			FOpening& Door = Wl.Openings[0];
			bool bBlocked = false;
			for (int32 m = 1; m + 1 < Meets.Num(); ++m)
			{
				bBlocked |= FMath::Abs(Meets[m] - Door.At) < Door.Width * 0.5f + Margin + 10.0f;
			}
			if (!bBlocked)
			{
				continue;
			}
			float BestLen = 0.0f, BestAt = Door.At;
			for (int32 m = 0; m + 1 < Meets.Num(); ++m)
			{
				const float Lo = Meets[m] + (m == 0 ? Margin : Margin);
				const float Hi = Meets[m + 1] - (m + 2 == Meets.Num() ? Margin : Margin);
				if (Hi - Lo > BestLen && Hi - Lo >= Door.Width + 20.0f)
				{
					BestLen = Hi - Lo;
					BestAt = (Lo + Hi) * 0.5f;
				}
			}
			Door.At = BestAt;
		}

		// Room types: ground floor living/kitchen/dining/bath; upper floor bedrooms/bath. Bigger rooms first.
		Rooms.Sort([](const FRect& A, const FRect& B) { return A.Area() > B.Area(); });
		TArray<ERoom> Types;
		for (int32 i = 0; i < Rooms.Num(); ++i)
		{
			ERoom T;
			if (Floor == 0)
			{
				T = i == 0 ? ERoom::Living : i == 1 ? ERoom::Kitchen : i == Rooms.Num() - 1 && Rooms.Num() > 2 ? ERoom::Bath : i == 2 ? ERoom::Dining : ERoom::Hall;
			}
			else
			{
				T = i == Rooms.Num() - 1 && Rooms.Num() > 1 ? ERoom::Bath : ERoom::Bedroom;
			}
			Types.Add(T);
		}

		// Exterior walls with windows (and the front door on the ground floor).
		for (int32 Side = 0; Side < 4; ++Side)
		{
			FWall Wall;
			Wall.bAlongX = Side >= 2;
			Wall.Fixed = Side == 0 ? -HX : Side == 1 ? HX : Side == 2 ? -HY : HY;
			Wall.From = Wall.bAlongX ? -HX : -HY;
			Wall.To = Wall.bAlongX ? HX : HY;
			Wall.Thickness = OuterWall;
			Wall.bExterior = true;
			const bool bFront = Side == 3 && Floor == 0;

			// Where interior walls meet this outer wall, so no window sits on one: windows are centred
			// in each room's stretch of the wall instead of spread evenly along it.
			TArray<float> Breaks;
			Breaks.Add(Wall.From);
			for (const FWall& Inner : Walls)
			{
				const bool bTouches =
					(Side == 0 && Inner.bAlongX && FMath::IsNearlyEqual(Inner.From, Inside.X0, 1.0f)) ||
					(Side == 1 && Inner.bAlongX && FMath::IsNearlyEqual(Inner.To, Inside.X1, 1.0f)) ||
					(Side == 2 && !Inner.bAlongX && FMath::IsNearlyEqual(Inner.From, Inside.Y0, 1.0f)) ||
					(Side == 3 && !Inner.bAlongX && FMath::IsNearlyEqual(Inner.To, Inside.Y1, 1.0f));
				if (bTouches)
				{
					Breaks.Add(Inner.Fixed);
				}
			}
			Breaks.Add(Wall.To);
			Breaks.Sort();
			const float Clearance = InnerWall * 0.5f + 30.0f;
			for (int32 b = 0; b + 1 < Breaks.Num(); ++b)
			{
				const float A = Breaks[b] + (b > 0 ? Clearance : OuterWall);
				const float Z1 = Breaks[b + 1] - (b + 2 < Breaks.Num() ? Clearance : OuterWall);
				const float Len = Z1 - A;
				if (Len < WindowWidth + 40.0f)
				{
					continue;
				}
				const int32 Count = FMath::Max(1, FMath::FloorToInt(Len / 300.0f));
				for (int32 k = 0; k < Count; ++k)
				{
					const float At = A + (k + 0.5f) * Len / Count;
					if (bFront && FMath::Abs(At - DoorX) < DoorWidth + WindowWidth * 0.5f)
					{
						continue;
					}
					// No window where the stair runs along the back wall.
					if (Side == 2 && At < Stair.X1 + WindowWidth * 0.5f)
					{
						continue;
					}
					Wall.Openings.Add({ At, WindowWidth, WindowSill, WindowSill + WindowHeight, true });
				}
			}
			if (bFront)
			{
				Wall.Openings.Add({ DoorX, DoorWidth, 0.0f, DoorHeight, false });
			}
			B.Wall(Wall, Z, Z + StoreyHeight - SlabThickness);
		}
		for (const FWall& Wall : Walls)
		{
			B.Wall(Wall, Z, Z + StoreyHeight - SlabThickness);
		}

		// Floor, with the stairwell left open above the ground floor.
		const ESurface FloorSurface = Floor == 0 ? ESurface::PlankFloor : ESurface::PlankFloor;
		if (Floor == 0)
		{
			B.Box(FloorSurface, FVector(Inside.X0, Inside.Y0, -SlabThickness), FVector(Inside.X1, Inside.Y1, 0.0f));
		}
		else
		{
			B.Box(FloorSurface, FVector(Stair.X1, Inside.Y0, Z - SlabThickness), FVector(Inside.X1, Inside.Y1, Z));
			B.Box(FloorSurface, FVector(Inside.X0, Stair.Y1, Z - SlabThickness), FVector(Stair.X1, Inside.Y1, Z));
		}
		// Bathrooms get tiles over the planks.
		for (int32 i = 0; i < Rooms.Num(); ++i)
		{
			if (Types[i] == ERoom::Bath)
			{
				B.Box(ESurface::TileFloor, FVector(Rooms[i].X0 + 6.0f, Rooms[i].Y0 + 6.0f, Z), FVector(Rooms[i].X1 - 6.0f, Rooms[i].Y1 - 6.0f, Z + 1.5f));
			}
		}

		// Stairs up to the next floor.
		if (Floor + 1 < Floors)
		{
			const int32 Steps = 15;
			const float Rise = StoreyHeight / Steps;
			const float Run = StairLength / Steps;
			for (int32 s = 0; s < Steps; ++s)
			{
				B.Box(ESurface::Wood, FVector(Stair.X0 + s * Run, Stair.Y0, Z + s * Rise), FVector(Stair.X0 + (s + 1) * Run + 4.0f, Stair.Y1, Z + (s + 1) * Rise));
			}
		}

		for (int32 i = 0; i < Rooms.Num(); ++i)
		{
			Furnish(B, Rooms[i], Types[i], Z, FloorRand, Walls);
		}
	}

	// Ceiling / roof base, then a pitched roof with gables: each gable is built from stacked courses
	// that step in, and each roof plane is a thin tilted slab overhanging the walls.
	const float Top = Floors * StoreyHeight - SlabThickness;
	// -NoRoof leaves the roofs off so a debug camera can look down into the rooms.
	if (FParse::Param(FCommandLine::Get(), TEXT("NoRoof")))
	{
		House.Height = Top;
		return House;
	}
	B.Box(ESurface::InteriorWall, FVector(-HX, -HY, Top), FVector(HX, HY, Top + SlabThickness));
	const float Pitch = 38.0f;
	const float RiseH = HY * FMath::Tan(FMath::DegreesToRadians(Pitch));
	const int32 Courses = 24;
	for (int32 c = 0; c < Courses; ++c)
	{
		const float Z0 = Top + SlabThickness + c * RiseH / Courses;
		const float Half = HY * (1.0f - (c + 0.5f) / Courses);
		for (const float Sx : { -1.0f, 1.0f })
		{
			B.Box(B.OuterSurface, FVector(Sx * HX - OuterWall * 0.5f, -Half, Z0), FVector(Sx * HX + OuterWall * 0.5f, Half, Z0 + RiseH / Courses));
		}
	}
	const float Slope = FMath::Sqrt(HY * HY + RiseH * RiseH) + 60.0f;
	const ESurface RoofSurface = Style == EStyle::Brick ? ESurface::SlateRoof : ESurface::ClayRoof;
	for (const float Sy : { -1.0f, 1.0f })
	{
		FPiece P;
		P.Surface = RoofSurface;
		P.Center = FVector(0.0f, Sy * HY * 0.5f, Top + SlabThickness + RiseH * 0.5f + 12.0f);
		P.Size = FVector(W + 80.0f, Slope, 14.0f);
		P.Rotation = FRotator(0.0f, 0.0f, Sy * Pitch);
		House.Pieces.Add(P);
	}
	// Corner boards up the walls, and a small window in each gable.
	for (const float Sx : { -1.0f, 1.0f })
	{
		for (const float Sy : { -1.0f, 1.0f })
		{
			B.Box(ESurface::Wood, FVector(Sx * HX - 9.0f + Sx * 6.0f, Sy * HY - 9.0f + Sy * 6.0f, 0.0f), FVector(Sx * HX + 9.0f + Sx * 6.0f, Sy * HY + 9.0f + Sy * 6.0f, Top));
		}
		const float Zc = Top + SlabThickness + RiseH * 0.28f;
		B.Box(ESurface::Wood, FVector(Sx * HX + Sx * 4.0f - 7.0f, -50.0f, Zc - 45.0f), FVector(Sx * HX + Sx * 4.0f + 7.0f, 50.0f, Zc + 45.0f));
		B.Box(ESurface::Glass, FVector(Sx * HX + Sx * 12.0f - 1.5f, -42.0f, Zc - 37.0f), FVector(Sx * HX + Sx * 12.0f + 1.5f, 42.0f, Zc + 37.0f));
	}
	// Chimney.
	B.Box(ESurface::Stone, FVector(HX * 0.45f, -40.0f, Top), FVector(HX * 0.45f + 70.0f, 30.0f, Top + RiseH + 120.0f));
	B.Box(ESurface::Stone, FVector(HX * 0.45f - 8.0f, -48.0f, Top + RiseH + 120.0f), FVector(HX * 0.45f + 78.0f, 38.0f, Top + RiseH + 145.0f));
	House.Height = Top + SlabThickness + RiseH;
	return House;
}
}
