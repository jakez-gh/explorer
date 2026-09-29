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
		bool bShutters = true;      // shutters and stone sills on outside windows
		bool bWhiteFrames = false;  // window and door frames painted white

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
				if (!O.bWindow)
				{
					// Keep 90 cm clear on both sides of every door.
					const float Clear = 90.0f;
					if (W.bAlongX) House.Doorways.Add(FVector4(A, W.Fixed - Clear, B, W.Fixed + Clear));
					else House.Doorways.Add(FVector4(W.Fixed - Clear, A, W.Fixed + Clear, B));
					House.DoorwayZ.Add(Z0);
				}
				// Frame and glass.
				const int32 FrameStart = House.Pieces.Num();
				const float Frame = 8.0f;
				if (O.bWindow)
				{
					Slab(ESurface::Wood, A, B, W.Fixed, W.Thickness + 4.0f, Z0 + O.Bottom - Frame, Z0 + O.Bottom, W.bAlongX);
					Slab(ESurface::Wood, A, B, W.Fixed, W.Thickness + 4.0f, Z0 + O.Top, Z0 + O.Top + Frame, W.bAlongX);
					Slab(ESurface::Wood, A - Frame, A, W.Fixed, W.Thickness + 4.0f, Z0 + O.Bottom, Z0 + O.Top, W.bAlongX);
					Slab(ESurface::Wood, B, B + Frame, W.Fixed, W.Thickness + 4.0f, Z0 + O.Bottom, Z0 + O.Top, W.bAlongX);
					Slab(ESurface::Wood, O.At - 3.0f, O.At + 3.0f, W.Fixed, 6.0f, Z0 + O.Bottom, Z0 + O.Top, W.bAlongX);
					Slab(ESurface::Glass, A, B, W.Fixed, 2.0f, Z0 + O.Bottom, Z0 + O.Top, W.bAlongX);
					if (W.bExterior && bShutters)
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
				if (bWhiteFrames)
				{
					for (int32 k = FrameStart; k < House.Pieces.Num(); ++k)
					{
						if (House.Pieces[k].Surface == ESurface::Wood) { House.Pieces[k].Surface = ESurface::ExteriorWall; House.Pieces[k].bTinted = true; House.Pieces[k].Tint = FLinearColor(2.4f, 2.4f, 2.4f); }
					}
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


	// Footprint (cm, model axes X by Y) of each scanned furniture model, from the loaded meshes.
	FVector2D FootprintOf(EFurniture F)
	{
		static const FVector2D Sizes[] = { {157,66},{85,77},{154,97},{137,58},{205,115},{43,58},{50,61},{119,55},{202,65},{149,204},{50,50},{86,44},{49,3},{100,26},{32,5},{117,134},{43,43} };
		return Sizes[static_cast<int32>(F)];
	}

	bool RectsOverlap(FVector2D CA, float YawA, FVector2D HA, FVector2D CB, float YawB, FVector2D HB)
	{
		auto Corners = [](FVector2D C, float Yaw, FVector2D H, FVector2D Out[4])
		{
			const float Cs = FMath::Cos(FMath::DegreesToRadians(Yaw)), Sn = FMath::Sin(FMath::DegreesToRadians(Yaw));
			const FVector2D Ax(Cs, Sn), Ay(-Sn, Cs);
			Out[0] = C + Ax * H.X + Ay * H.Y; Out[1] = C - Ax * H.X + Ay * H.Y; Out[2] = C - Ax * H.X - Ay * H.Y; Out[3] = C + Ax * H.X - Ay * H.Y;
		};
		FVector2D PA[4], PB[4];
		Corners(CA, YawA, HA, PA);
		Corners(CB, YawB, HB, PB);
		const FVector2D Axes[4] = { PA[0] - PA[1], PA[0] - PA[3], PB[0] - PB[1], PB[0] - PB[3] };
		for (const FVector2D& A0 : Axes)
		{
			const FVector2D Ax = A0.GetSafeNormal();
			float MinA = 1e9f, MaxA = -1e9f, MinB = 1e9f, MaxB = -1e9f;
			for (int32 k = 0; k < 4; ++k)
			{
				const float DA = FVector2D::DotProduct(PA[k], Ax), DB = FVector2D::DotProduct(PB[k], Ax);
				MinA = FMath::Min(MinA, DA); MaxA = FMath::Max(MaxA, DA); MinB = FMath::Min(MinB, DB); MaxB = FMath::Max(MaxB, DB);
			}
			if (MaxA < MinB + 1.0f || MaxB < MinA + 1.0f) return false;
		}
		return true;
	}

	// Drop any piece of furniture that would sit inside another, poke through a wall or block a doorway.
	void PruneFurniture(FHouse& House)
	{
		struct FKept { FVector2D C; float Yaw; FVector2D H; float Z; };
		TArray<FKept> Kept;
		TArray<FPiece> Out;
		for (const FPiece& P : House.Pieces)
		{
			if (!P.bFurniture) { Out.Add(P); continue; }
			const FVector2D Foot = FootprintOf(P.Furniture);
			const bool bWallHung = P.Furniture == EFurniture::Mirror || P.Furniture == EFurniture::Clock || P.Furniture == EFurniture::CeilingLamp || P.Furniture == EFurniture::Shelf;
			if (bWallHung) { Out.Add(P); continue; }
			const FVector2D C(P.Center.X, P.Center.Y);
			const FVector2D H = Foot * 0.5f;
			bool bOk = true;
			for (const FKept& K : Kept)
			{
				if (FMath::Abs(K.Z - P.Center.Z) < 150.0f && RectsOverlap(C, P.Rotation.Yaw, H, K.C, K.Yaw, K.H)) { bOk = false; break; }
			}
			for (int32 d = 0; bOk && d < House.Doorways.Num(); ++d)
			{
				const FVector4& D = House.Doorways[d];
				if (FMath::Abs(House.DoorwayZ[d] - P.Center.Z) < 150.0f && RectsOverlap(C, P.Rotation.Yaw, H, FVector2D((D.X + D.Z) * 0.5f, (D.Y + D.W) * 0.5f), 0.0f, FVector2D((D.Z - D.X) * 0.5f, (D.W - D.Y) * 0.5f))) bOk = false;
			}
			for (const FPiece& W : House.Pieces)
			{
				if (!bOk) break;
				if (W.bFurniture || W.Size.Z < 150.0f || FMath::Min(W.Size.X, W.Size.Y) > 40.0f) continue;                 // thin, tall pieces are walls
				if (P.Center.Z + 100.0f < W.Center.Z - W.Size.Z * 0.5f || P.Center.Z > W.Center.Z + W.Size.Z * 0.5f) continue; // other floor
				if (RectsOverlap(C, P.Rotation.Yaw, H, FVector2D(W.Center.X, W.Center.Y), W.Rotation.Yaw, FVector2D(W.Size.X, W.Size.Y) * 0.5f)) bOk = false;
			}
			if (bOk)
			{
				Kept.Add({ C, static_cast<float>(P.Rotation.Yaw), H, static_cast<float>(P.Center.Z) });
				Out.Add(P);
			}
		}
		House.Pieces = Out;
	}

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
			// Chairs sit outside the 205 x 115 cm table, tucked to its long sides.
			B.Furniture(EFurniture::DiningChair, FVector(M.X - 45.0f, M.Y - 92.0f, Z), 90.0f);
			B.Furniture(EFurniture::DiningChair, FVector(M.X + 45.0f, M.Y - 92.0f, Z), 90.0f);
			B.Furniture(EFurniture::DiningChair, FVector(M.X - 45.0f, M.Y + 92.0f, Z), -90.0f);
			B.Furniture(EFurniture::DiningChair, FVector(M.X + 45.0f, M.Y + 92.0f, Z), -90.0f);
			break;
		case ERoom::Dining:
			B.Furniture(EFurniture::DiningTable, FVector(M.X, M.Y, Z), 0.0f);
			B.Furniture(EFurniture::DiningChair, FVector(M.X - 50.0f, M.Y - 92.0f, Z), 90.0f);
			B.Furniture(EFurniture::DiningChair, FVector(M.X + 50.0f, M.Y - 92.0f, Z), 90.0f);
			B.Furniture(EFurniture::DiningChair, FVector(M.X - 50.0f, M.Y + 92.0f, Z), -90.0f);
			B.Furniture(EFurniture::DiningChair, FVector(M.X + 50.0f, M.Y + 92.0f, Z), -90.0f);
			B.Furniture(EFurniture::DiningChair, FVector(M.X - 136.0f, M.Y, Z), 0.0f);
			B.Furniture(EFurniture::DiningChair, FVector(M.X + 136.0f, M.Y, Z), 180.0f);
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

FHouse Generate(uint32 Seed, EStyle Style, bool bTwoStoreys, float ForceWidth, float ForceDepth, bool bFlatRoof, int32 ForceFloors)
{
	FHouse House;
	FRand Rand{ Seed };
	const float RandW = FMath::GridSnap(Rand.Range(800.0f, 1300.0f), 10.0f);
	const float RandD = FMath::GridSnap(Rand.Range(650.0f, 1000.0f), 10.0f);
	const float W = ForceWidth > 0.0f ? FMath::Max(ForceWidth, 560.0f) : RandW;
	const float D = ForceDepth > 0.0f ? FMath::Max(ForceDepth, 520.0f) : RandD;
	const int32 Floors = ForceFloors > 0 ? ForceFloors : bTwoStoreys ? 2 : 1;
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
		PruneFurniture(House); return House;
	}
	B.Box(ESurface::InteriorWall, FVector(-HX, -HY, Top), FVector(HX, HY, Top + SlabThickness));
	if (bFlatRoof)
	{
		// Flat roof behind a parapet.
		const float Roof = Top + SlabThickness;
		B.Box(ESurface::SlateRoof, FVector(-HX - 10.0f, -HY - 10.0f, Roof), FVector(HX + 10.0f, HY + 10.0f, Roof + 8.0f));
		B.Box(B.OuterSurface, FVector(-HX - 11.0f, -HY - 11.0f, Roof), FVector(HX + 11.0f, -HY + 11.0f, Roof + 70.0f));
		B.Box(B.OuterSurface, FVector(-HX - 11.0f, HY - 11.0f, Roof), FVector(HX + 11.0f, HY + 11.0f, Roof + 70.0f));
		B.Box(B.OuterSurface, FVector(-HX - 11.0f, -HY + 11.0f, Roof), FVector(-HX + 11.0f, HY - 11.0f, Roof + 70.0f));
		B.Box(B.OuterSurface, FVector(HX - 11.0f, -HY + 11.0f, Roof), FVector(HX + 11.0f, HY - 11.0f, Roof + 70.0f));
		House.Height = Roof + 70.0f;
		PruneFurniture(House); return House;
	}
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
	PruneFurniture(House); return House;
}
FHouse GenerateFamilyHouse1719()
{
	// 1719 Avenue E, Council Bluffs, as the family remembers it: a story-and-a-half with blue vinyl siding and brown trim,
	// concrete front steps, an enclosed front porch, a blue-and-brown garage on an asphalt pad, a chain-link fence, one tree
	// out back and a full cinderblock basement. Ground floor: a living room across the front (no dining room; the family ate at a four-top in the kitchen), kitchen with a pantry and a whole
	// bath beside it, the master bedroom, and the baby's room (once a den), all round a central hall with the stairs.
	// Upstairs: the boys' room open to the landing and the girl's room in the dormer. Basement: the teen's bedroom, the
	// laundry, black-and-orange tile and a bar. A 220 V window air conditioner sits in the living room; interiors are earth
	// tones. Local frame: +Y is the street; the ground floor is Z = 0.
	FHouse House;
	const float HX = 450.0f, HY = 550.0f;
	const float Storey = StoreyHeight;
	const float Wall = Storey - SlabThickness;
	const float Knee = 130.0f;
	House.Width = HX * 2.0f;
	House.Depth = HY * 2.0f;
	FBuilder B{ House, ESurface::Siding };
	B.bShutters = false;
	B.bWhiteFrames = true;
	auto Box = [&](ESurface S, float X0, float Y0, float Z0, float X1, float Y1, float Z1)
	{
		B.Box(S, FVector(FMath::Min(X0, X1), FMath::Min(Y0, Y1), Z0), FVector(FMath::Max(X0, X1), FMath::Max(Y0, Y1), Z1));
	};
	auto Tinted = [&](ESurface S, const FLinearColor& C, float X0, float Y0, float Z0, float X1, float Y1, float Z1)
	{
		Box(S, X0, Y0, Z0, X1, Y1, Z1);
		House.Pieces.Last().bTinted = true;
		House.Pieces.Last().Tint = C;
	};
	auto Put = [&](EFurniture F, float X, float Y, float Yaw, float Z = 0.0f) { B.Furniture(F, FVector(X, Y, Z), Yaw); };
	const FLinearColor Black(0.03f, 0.03f, 0.03f), Orange(1.6f, 0.45f, 0.06f), Cinder(0.95f, 0.95f, 0.92f), AsphaltC(0.12f, 0.12f, 0.13f);

	struct FHole { float X0, Y0, X1, Y1; };
	// Brown carpet with 1-3 inch roundish spots of tan and darker brown (a procedural material), the same on every floor
	// except the kitchen and bathrooms, and on the stairs going up. Areas touching a Skip rectangle are left bare.
	auto Carpet = [&](float X0, float Y0, float X1, float Y1, float Z, std::initializer_list<FHole> Skips)
	{
		for (float X = X0; X < X1 - 1.0f; X += 60.0f)
		{
			for (float Y = Y0; Y < Y1 - 1.0f; Y += 60.0f)
			{
				const float A1 = FMath::Min(X + 60.0f, X1), B1 = FMath::Min(Y + 60.0f, Y1);
				bool bSkip = false;
				for (const FHole& H : Skips) bSkip |= A1 > H.X0 && X < H.X1 && B1 > H.Y0 && Y < H.Y1;
				if (!bSkip) Box(ESurface::Carpet, X, Y, Z, A1, B1, Z + 1.0f);
			}
		}
	};
	auto FloorSlab = [&](ESurface S, float Z0, float Z1, std::initializer_list<FHole> Holes)
	{
		TArray<float> Xs, Ys;
		Xs.Add(-HX + 11.0f); Xs.Add(HX - 11.0f); Ys.Add(-HY + 11.0f); Ys.Add(HY - 11.0f);
		for (const FHole& H : Holes) { Xs.Add(H.X0); Xs.Add(H.X1); Ys.Add(H.Y0); Ys.Add(H.Y1); }
		Xs.Sort(); Ys.Sort();
		for (int32 i = 0; i + 1 < Xs.Num(); ++i)
		{
			for (int32 j = 0; j + 1 < Ys.Num(); ++j)
			{
				const float CX = (Xs[i] + Xs[i + 1]) * 0.5f, CY = (Ys[j] + Ys[j + 1]) * 0.5f;
				bool bHole = false;
				for (const FHole& H : Holes) bHole |= CX > H.X0 && CX < H.X1 && CY > H.Y0 && CY < H.Y1;
				if (!bHole) Box(S, Xs[i], Ys[j], Z0, Xs[i + 1], Ys[j + 1], Z1);
			}
		}
	};
	auto Exterior = [&](bool bAlongX, float Fixed, float Z0, float Height, std::initializer_list<FOpening> Openings)
	{
		FWall Ex;
		Ex.bAlongX = bAlongX; Ex.Fixed = Fixed;
		Ex.From = bAlongX ? -HX : -HY; Ex.To = bAlongX ? HX : HY;
		Ex.Thickness = OuterWall; Ex.bExterior = true;
		for (const FOpening& O : Openings) Ex.Openings.Add(O);
		B.Wall(Ex, Z0, Z0 + Height);
	};
	auto Interior = [&](bool bAlongX, float Fixed, float From, float To, float Z0, float Height, std::initializer_list<float> Doors)
	{
		FWall In;
		In.bAlongX = bAlongX; In.Fixed = Fixed; In.From = From; In.To = To;
		In.Thickness = InnerWall; In.bExterior = false;
		for (const float At : Doors) In.Openings.Add({ At, 90.0f, 0.0f, DoorHeight, false });
		B.Wall(In, Z0, Z0 + Height);
	};
	auto GableRoof = [&](float CX, float CY, float RX, float RY, float Top, float Pitch, ESurface Roof)
	{
		const float Rise = RY * FMath::Tan(FMath::DegreesToRadians(Pitch));
		const int32 Courses = 24;
		for (int32 c = 0; c < Courses; ++c)
		{
			const float Z0 = Top + c * Rise / Courses;
			const float Half = RY * (1.0f - (c + 0.5f) / Courses);
			for (const float Sx : { -1.0f, 1.0f })
			{
				B.Box(B.OuterSurface, FVector(CX + Sx * RX - OuterWall * 0.5f, CY - Half, Z0), FVector(CX + Sx * RX + OuterWall * 0.5f, CY + Half, Z0 + Rise / Courses));
			}
		}
		const float Slope = FMath::Sqrt(RY * RY + Rise * Rise) + 70.0f;
		for (const float Sy : { -1.0f, 1.0f })
		{
			FPiece P;
			P.Surface = Roof;
			P.Center = FVector(CX, CY + Sy * RY * 0.5f, Top + Rise * 0.5f + 12.0f);
			P.Size = FVector(RX * 2.0f + 90.0f, Slope, 12.0f);
			P.Rotation = FRotator(0.0f, 0.0f, Sy * Pitch);
			House.Pieces.Add(P);
		}
		return Rise;
	};
	// Front-gable roof: the ridge runs from the street to the back yard, so the gable ends face the street and the yard.
	auto GableRoofY = [&](float CX, float CY, float RX, float RY, float Top, float Pitch, ESurface Roof)
	{
		const float Rise = RX * FMath::Tan(FMath::DegreesToRadians(Pitch));
		const int32 Courses = 28;
		for (int32 c = 0; c < Courses; ++c)
		{
			const float Z0 = Top + c * Rise / Courses;
			const float Half = RX * (1.0f - (c + 0.5f) / Courses);
			for (const float Sy : { -1.0f, 1.0f })
			{
				B.Box(B.OuterSurface, FVector(CX - Half, CY + Sy * RY - OuterWall * 0.5f, Z0), FVector(CX + Half, CY + Sy * RY + OuterWall * 0.5f, Z0 + Rise / Courses));
			}
		}
		const float Slope = FMath::Sqrt(RX * RX + Rise * Rise) + 70.0f;
		for (const float Sx : { -1.0f, 1.0f })
		{
			FPiece P;
			P.Surface = Roof;
			P.Center = FVector(CX + Sx * RX * 0.5f, CY, Top + Rise * 0.5f + 12.0f);
			P.Size = FVector(RY * 2.0f + 90.0f, Slope, 12.0f);
			P.Rotation = FRotator(0.0f, 90.0f, -Sx * Pitch);
			House.Pieces.Add(P);
		}
		return Rise;
	};
	const float WinB = 90.0f, WinT = 220.0f;
	const float Z1 = Storey;
	// The stairs up climb toward the back of the house, ending at a door to the outside. The cellar stairs go down
	// from the back room on the east side.
	const FHole UpStair{ -45.0f, -460.0f, 45.0f, -100.0f };
	const FHole DownStair{ 65.0f, -525.0f, 425.0f, -415.0f };
	const float BZ = -Storey;

	// ================= Basement (cinderblock) =================
	Box(ESurface::Stone, -HX - 12.0f, -HY - 12.0f, BZ - 40.0f, HX + 12.0f, HY + 12.0f, BZ);
	for (const float S : { -1.0f, 1.0f })
	{
		Tinted(ESurface::Stone, Cinder, -HX - 11.0f, S * HY - 11.0f, BZ, HX + 11.0f, S * HY + 11.0f, 0.0f);
		Tinted(ESurface::Stone, Cinder, S * HX - 11.0f, -HY + 11.0f, BZ, S * HX + 11.0f, HY - 11.0f, 0.0f);
	}
	for (int32 i = 0; i < 15; ++i)
	{
		for (int32 j = 0; j < 18; ++j)
		{
			const float X0 = -HX + 11.0f + i * 59.0f, Y0 = -HY + 11.0f + j * 60.0f;
			Tinted(ESurface::TileFloor, ((i + j) & 1) ? Orange : Black, X0, Y0, BZ, FMath::Min(X0 + 59.0f, HX - 11.0f), FMath::Min(Y0 + 60.0f, HY - 11.0f), BZ + 2.0f);
		}
	}
	// Cellar stairs: down from the back room, running east.
	for (int32 s = 0; s < 15; ++s)
	{
		const float Run = 360.0f / 15.0f, Rise = Storey / 15.0f;
		Box(ESurface::Wood, 65.0f + s * Run, -525.0f, BZ + 2.0f, 65.0f + (s + 1) * Run, -415.0f, -(s + 1) * Rise + 2.0f);
	}
	// Teen's bedroom (back-left) built as a cinderblock room.
	auto CinderWall = [&](bool bAlongX, float Fixed, float From, float To, std::initializer_list<float> Doors)
	{
		FWall W;
		W.bAlongX = bAlongX; W.Fixed = Fixed; W.From = From; W.To = To; W.Thickness = InnerWall + 4.0f; W.bExterior = false;
		for (const float At : Doors) W.Openings.Add({ At, 90.0f, 0.0f, DoorHeight, false });
		B.Wall(W, BZ, -SlabThickness);
	};
	CinderWall(true, -230.0f, -HX + 11.0f, -140.0f, {});
	CinderWall(false, -140.0f, -HY + 11.0f, -230.0f, { -380.0f });
	// Teen's room.
	Tinted(ESurface::InteriorWall, FLinearColor(0.8f, 0.72f, 0.6f), -HX + 14.0f, -460.0f, BZ, -HX + 204.0f, -320.0f, BZ + 26.0f);   // mattress on the floor
	Put(EFurniture::Nightstand, -HX + 40.0f, -300.0f, 0.0f, BZ);
	Box(ESurface::Wood, -360.0f, -340.0f, BZ + 72.0f, -300.0f, -230.0f + 0.0f, BZ + 78.0f);
	Put(EFurniture::DiningChair, -330.0f, -290.0f, 0.0f, BZ);
	Put(EFurniture::Drawers, -230.0f, -520.0f, 90.0f, BZ);
	Put(EFurniture::Mirror, -160.0f, -300.0f, 180.0f, BZ + 100.0f);
	Put(EFurniture::CeilingLamp, -290.0f, -390.0f, 0.0f, -SlabThickness - 45.0f);
	// Laundry, out in the open along the west wall, and the water heater.
	Box(ESurface::InteriorWall, -HX + 14.0f, -160.0f, BZ, -HX + 80.0f, -95.0f, BZ + 95.0f);
	Box(ESurface::InteriorWall, -HX + 14.0f, -85.0f, BZ, -HX + 80.0f, -20.0f, BZ + 95.0f);
	Box(ESurface::Wood, -HX + 14.0f, -10.0f, BZ, -HX + 80.0f, 70.0f, BZ + 90.0f);
	Box(ESurface::Stone, 200.0f, -350.0f, BZ, 255.0f, -310.0f, BZ + 140.0f);
	// Rec room with the bar.
	Box(ESurface::Wood, 230.0f, 60.0f, BZ, 330.0f, 420.0f, BZ + 108.0f);
	Box(ESurface::Wood, 225.0f, 55.0f, BZ + 108.0f, 335.0f, 425.0f, BZ + 114.0f);
	Box(ESurface::Wood, 330.0f, 320.0f, BZ, 430.0f, 420.0f, BZ + 108.0f);
	Box(ESurface::Wood, 330.0f, 315.0f, BZ + 108.0f, 435.0f, 425.0f, BZ + 114.0f);
	for (const float Z : { 110.0f, 150.0f, 190.0f })
	{
		Box(ESurface::Wood, HX - 44.0f, 60.0f, BZ + Z, HX - 12.0f, 300.0f, BZ + Z + 5.0f);
		for (int32 k = 0; k < 8; ++k) Box(ESurface::Glass, HX - 38.0f, 75.0f + k * 28.0f, BZ + Z + 5.0f, HX - 26.0f, 87.0f + k * 28.0f, BZ + Z + 30.0f);
	}
	for (int32 k = 0; k < 4; ++k) Box(ESurface::Wood, 180.0f, 95.0f + k * 85.0f, BZ, 215.0f, 130.0f + k * 85.0f, BZ + 72.0f);
	Put(EFurniture::Sofa, -330.0f, 250.0f, 0.0f, BZ);
	Put(EFurniture::CoffeeTable, -140.0f, 250.0f, 0.0f, BZ);
	Put(EFurniture::ArmChair, -160.0f, 420.0f, 200.0f, BZ);
	Tinted(ESurface::Wood, Black, -HX + 14.0f, 380.0f, BZ, -HX + 60.0f, 460.0f, BZ + 45.0f);
	Tinted(ESurface::Wood, Black, -HX + 20.0f, 385.0f, BZ + 45.0f, -HX + 32.0f, 455.0f, BZ + 100.0f);
	Put(EFurniture::CeilingLamp, -100.0f, 250.0f, 0.0f, -SlabThickness - 45.0f);
	Put(EFurniture::CeilingLamp, 250.0f, 250.0f, 0.0f, -SlabThickness - 45.0f);
	Put(EFurniture::CeilingLamp, 0.0f, -100.0f, 0.0f, -SlabThickness - 45.0f);

	// ================= Ground floor =================
	FloorSlab(ESurface::PlankFloor, -SlabThickness, 0.0f, { DownStair });
	Box(ESurface::TileFloor, 55.0f, -60.0f, 0.0f, HX - 12.0f, 265.0f, 1.5f);           // kitchen
	Box(ESurface::TileFloor, 55.0f, -300.0f, 0.0f, HX - 12.0f, -65.0f, 1.5f);         // whole bath
	Exterior(true, HY, 0.0f, Wall, { { 0.0f, DoorWidth, 0.0f, DoorHeight, false }, { -330.0f, 110.0f, WinB, WinT, true }, { 290.0f, 130.0f, WinB, WinT, true } });
	Exterior(true, -HY, 0.0f, Wall, { { -250.0f, DoorWidth, 0.0f, DoorHeight, false }, { 250.0f, 100.0f, WinB, WinT, true } });
	Exterior(false, -HX, 0.0f, Wall, { { 400.0f, 130.0f, WinB, WinT, true }, { 110.0f, 110.0f, WinB, WinT, true }, { -180.0f, 100.0f, WinB, WinT, true } });
	Exterior(false, HX, 0.0f, Wall, { { 400.0f, 120.0f, WinB, WinT, true }, { 110.0f, 100.0f, WinB, WinT, true }, { -130.0f, 60.0f, 140.0f, 210.0f, true }, { -250.0f, 60.0f, 140.0f, 210.0f, true } });
	// Central hall (x -50..50): stairs up in the middle of the house, cellar door at the back.
	Interior(false, -50.0f, -528.0f, 270.0f, 0.0f, Wall, { 150.0f, 20.0f });   // the stairs fill the hall behind y = -100, so doors stay in front of them
	Interior(false, 50.0f, -528.0f, 270.0f, 0.0f, Wall, { 150.0f, 20.0f });
	Interior(true, 270.0f, -HX + 11.0f, -50.0f, 0.0f, Wall, {});                        // master bedroom's front wall (living beyond)
	Interior(true, 270.0f, 50.0f, 150.0f, 0.0f, Wall, {});                              // kitchen / family room, then the bar
	Box(ESurface::Wood, 150.0f, 250.0f, 0.0f, HX - 12.0f, 290.0f, 100.0f);             // breakfast bar counter base
	Box(ESurface::Wood, 150.0f, 245.0f, 100.0f, HX - 12.0f, 305.0f, 106.0f);           // its top, overhanging the family-room side
	for (const float X : { 195.0f, 270.0f, 345.0f }) Box(ESurface::Wood, X - 18.0f, 328.0f, 0.0f, X + 18.0f, 364.0f, 68.0f);   // stools
	Interior(true, -60.0f, -HX + 11.0f, -50.0f, 0.0f, Wall, { -250.0f });                // master / baby's room
	Interior(true, -300.0f, -HX + 11.0f, -50.0f, 0.0f, Wall, {});                        // baby's room / back entry
	Interior(true, -60.0f, 50.0f, HX - 11.0f, 0.0f, Wall, { 200.0f });                   // kitchen / pantry
	Interior(true, -205.0f, 50.0f, HX - 11.0f, 0.0f, Wall, {});                          // pantry / whole bath
	Interior(true, -300.0f, 50.0f, HX - 11.0f, 0.0f, Wall, {});                          // bath / storage
	// Main stairs up, straight up the middle of the house toward the back wall; carpeted.
	for (int32 s = 0; s < 15; ++s)
	{
		const float Run = 360.0f / 15.0f, Rise = Storey / 15.0f;
		Box(ESurface::Carpet, -45.0f, -100.0f - (s + 1) * Run - 3.0f, s * Rise, 45.0f, -100.0f - s * Run, (s + 1) * Rise);
	}
	// Living room (west, front) with the window air conditioner; dining room (east, front).
	Put(EFurniture::Sofa, -150.0f, 440.0f, 180.0f);
	Put(EFurniture::ArmChair, -300.0f, 330.0f, 150.0f);
	Put(EFurniture::ArmChair, -60.0f, 340.0f, 200.0f);
	Carpet(-HX + 12.0f, -HY + 12.0f, HX - 12.0f, HY - 12.0f, 0.0f, { DownStair, FHole{ 50.0f, -300.0f, HX, 270.0f } });
	Tinted(ESurface::Wood, Black, -HX + 14.0f, 470.0f, 0.0f, -HX + 54.0f, 520.0f, 45.0f);
	Tinted(ESurface::Wood, Black, -HX + 20.0f, 472.0f, 45.0f, -HX + 32.0f, 518.0f, 100.0f);
	Tinted(ESurface::InteriorWall, FLinearColor(1.2f, 1.2f, 1.15f), -HX - 30.0f, 335.0f, WinB + 30.0f, -HX + 40.0f, 465.0f, WinB + 105.0f); // window AC, 220 V
	Box(ESurface::Wood, -HX + 12.0f, 470.0f, 25.0f, -HX + 22.0f, 480.0f, 55.0f);
	// No dining room: the family ate at a four-top in the kitchen, and the front room is one living room.
	// Lived-in: end tables and lamps, toys, shoes by the door, a laundry basket, a stack of mail and books.
	Box(ESurface::Wood, -60.0f, 450.0f, 0.0f, -20.0f, 490.0f, 52.0f);
	Box(ESurface::Wood, -300.0f, 300.0f, 0.0f, -260.0f, 340.0f, 52.0f);
	Box(ESurface::Wood, -55.0f, 455.0f, 52.0f, -25.0f, 485.0f, 100.0f);
	Box(ESurface::Wood, -170.0f, 285.0f, 0.0f, -120.0f, 315.0f, 6.0f);    // toys on the floor
	Box(ESurface::Stone, -110.0f, 315.0f, 0.0f, -85.0f, 340.0f, 20.0f);
	Box(ESurface::Wood, 60.0f, 330.0f, 0.0f, 100.0f, 350.0f, 6.0f);        // a pair of shoes
	Box(ESurface::Wood, 280.0f, 450.0f, 0.0f, 340.0f, 500.0f, 35.0f);      // laundry basket
	Box(ESurface::Wood, 60.0f, 500.0f, 0.0f, 120.0f, 520.0f, 120.0f);      // coat hooks and coats by the front door
	Put(EFurniture::CeilingLamp, -180.0f, 400.0f, 0.0f, Wall - 45.0f);
	Put(EFurniture::CeilingLamp, 250.0f, 400.0f, 0.0f, Wall - 45.0f);
	// Kitchen, pantry, whole bath.
	// The kitchen four-top: a 90 cm square table (boxes) with four chairs pulled up, clear of the doorway and cabinets.
	Box(ESurface::Wood, 175.0f, 85.0f, 72.0f, 265.0f, 175.0f, 77.0f);
	for (const float X : { 180.0f, 258.0f }) for (const float Y : { 90.0f, 168.0f }) Box(ESurface::Wood, X - 2.5f, Y - 2.5f, 0.0f, X + 2.5f, Y + 2.5f, 72.0f);
	Put(EFurniture::DiningChair, 220.0f, 60.0f, 90.0f);
	Put(EFurniture::DiningChair, 220.0f, 200.0f, -90.0f);
	Put(EFurniture::DiningChair, 150.0f, 130.0f, 0.0f);
	Put(EFurniture::DiningChair, 290.0f, 130.0f, 180.0f);
	Put(EFurniture::Stove, 420.0f, 110.0f, 180.0f);
	Put(EFurniture::KitchenCabinet, 220.0f, -30.0f, 90.0f);
	Put(EFurniture::KitchenCabinet, 320.0f, -30.0f, 90.0f);
	Box(ESurface::InteriorWall, 372.0f, 195.0f, 0.0f, 434.0f, 255.0f, 180.0f);   // refrigerator, out of the doorway
	Put(EFurniture::CeilingLamp, 250.0f, 110.0f, 0.0f, Wall - 45.0f);
	Put(EFurniture::Shelf, 250.0f, -195.0f, 90.0f);
	Put(EFurniture::Shelf, 420.0f, -130.0f, 180.0f);
	Box(ESurface::InteriorWall, 300.0f, -290.0f, 0.0f, HX - 12.0f, -215.0f, 55.0f);        // tub
	Box(ESurface::InteriorWall, 80.0f, -260.0f, 0.0f, 115.0f, -225.0f, 40.0f);              // toilet
	Box(ESurface::InteriorWall, 190.0f, -290.0f, 0.0f, 240.0f, -262.0f, 85.0f);              // sink
	Put(EFurniture::Mirror, 215.0f, -262.0f, 90.0f, 110.0f);
	Put(EFurniture::CeilingLamp, 150.0f, -250.0f, 0.0f, Wall - 45.0f);
	// Master bedroom (west, middle) and the baby's room (west, back; once the den).
	Tinted(ESurface::InteriorWall, FLinearColor(0.85f, 0.78f, 0.66f), -HX + 14.0f, 30.0f, 0.0f, -HX + 204.0f, 170.0f, 26.0f);   // mattress, no visible frame
	Put(EFurniture::Nightstand, -HX + 40.0f, 0.0f, 0.0f);
	Put(EFurniture::Nightstand, -HX + 40.0f, 200.0f, 0.0f);
	Put(EFurniture::Drawers, -230.0f, 225.0f, -90.0f);
	Put(EFurniture::Mirror, -100.0f, 240.0f, -90.0f, 100.0f);
	Put(EFurniture::CeilingLamp, -240.0f, 100.0f, 0.0f, Wall - 45.0f);
	Box(ESurface::Wood, -HX + 14.0f, -240.0f, 20.0f, -HX + 154.0f, -170.0f, 25.0f);          // crib
	Box(ESurface::Wood, -HX + 14.0f, -240.0f, 25.0f, -HX + 18.0f, -170.0f, 95.0f);
	Box(ESurface::Wood, -HX + 150.0f, -240.0f, 25.0f, -HX + 154.0f, -170.0f, 95.0f);
	Box(ESurface::Wood, -HX + 14.0f, -240.0f, 25.0f, -HX + 154.0f, -236.0f, 95.0f);
	Box(ESurface::Wood, -HX + 14.0f, -174.0f, 25.0f, -HX + 154.0f, -170.0f, 95.0f);
	Put(EFurniture::Drawers, -230.0f, -100.0f, -90.0f);
	Put(EFurniture::CeilingLamp, -240.0f, -180.0f, 0.0f, Wall - 45.0f);
	// Back entry and storage.
	Put(EFurniture::Shelf, -250.0f, -500.0f, 90.0f);

	// ================= Upper floor: a half-storey under the roof =================
	FloorSlab(ESurface::PlankFloor, Z1 - SlabThickness, Z1, { UpStair });
	Carpet(-HX + 12.0f, -HY + 12.0f, HX - 12.0f, HY - 12.0f, Z1, { UpStair });
	Exterior(true, HY, Z1, Knee, {});
	Exterior(true, -HY, Z1, Knee, {});
	Exterior(false, -HX, Z1, Knee, {});
	Exterior(false, HX, Z1, Knee, {});
	Interior(false, -50.0f, -290.0f, 300.0f, Z1, 300.0f, { });                          // boys' room is open to the landing on this side
	Interior(false, 50.0f, -290.0f, 300.0f, Z1, 300.0f, { 100.0f });
	// Left of the top of the stairs (facing the back door): a small carpeted closet with a toilet in it.
	Interior(false, 180.0f, -535.0f, -430.0f, Z1, 200.0f, {});
	Interior(true, -430.0f, 50.0f, 180.0f, Z1, 200.0f, {});
	Interior(false, 50.0f, -535.0f, -430.0f, Z1, 200.0f, { -490.0f });
	Box(ESurface::InteriorWall, 130.0f, -520.0f, Z1, 165.0f, -480.0f, Z1 + 40.0f);             // the toilet
	Box(ESurface::InteriorWall, 138.0f, -480.0f, Z1 + 40.0f, 160.0f, -472.0f, Z1 + 95.0f);      // its tank
	// The back door upstairs: a door in the rear wall that opens on nothing, just a drop to the yard.
	{
		FWall Back; Back.bAlongX = true; Back.Fixed = -HY; Back.From = -70.0f; Back.To = 70.0f; Back.Thickness = OuterWall; Back.bExterior = true;
		Back.Openings.Add({ 0.0f, 100.0f, 0.0f, DoorHeight, false });
		B.Wall(Back, Z1, Z1 + 260.0f);
		Box(ESurface::ExteriorWall, -70.0f, -HY - 11.0f, Z1, -58.0f, -HY + 120.0f, Z1 + 275.0f);
		Box(ESurface::ExteriorWall, 58.0f, -HY - 11.0f, Z1, 70.0f, -HY + 120.0f, Z1 + 275.0f);
		FPiece Cap; Cap.Surface = ESurface::SlateRoof; Cap.Center = FVector(0.0f, -HY + 55.0f, Z1 + 282.0f); Cap.Size = FVector(170.0f, 200.0f, 8.0f);
		House.Pieces.Add(Cap);
		Box(ESurface::Wood, -60.0f, -HY - 9.0f, Z1 - 4.0f, 60.0f, -HY + 12.0f, Z1);                       // sill, then the drop
	}
	// Railing around the stair opening (the stairs climb toward the back door).
	Box(ESurface::Wood, -46.0f, -464.0f, Z1, -40.0f, -94.0f, Z1 + 95.0f);
	Box(ESurface::Wood, 40.0f, -464.0f, Z1, 46.0f, -94.0f, Z1 + 95.0f);
	Box(ESurface::Wood, -46.0f, -100.0f, Z1, 46.0f, -94.0f, Z1 + 95.0f);
	// Boys' room (west): two mattresses on the floor, no bunks or visible frames; toys.
	Tinted(ESurface::InteriorWall, FLinearColor(0.55f, 0.65f, 1.1f), -HX + 14.0f, 10.0f, Z1, -HX + 204.0f, 100.0f, Z1 + 22.0f);
	Tinted(ESurface::InteriorWall, FLinearColor(1.05f, 0.55f, 0.45f), -HX + 14.0f, 120.0f, Z1, -HX + 204.0f, 210.0f, Z1 + 22.0f);
	Box(ESurface::Wood, -HX + 12.0f, -200.0f, Z1, -HX + 82.0f, -150.0f, Z1 + 45.0f);      // toy chest
	Box(ESurface::Stone, -300.0f, -100.0f, Z1, -270.0f, -70.0f, Z1 + 22.0f);
	Put(EFurniture::Drawers, -HX + 40.0f, -60.0f, 0.0f, Z1);
	Put(EFurniture::CeilingLamp, -240.0f, 0.0f, 0.0f, Z1 + 200.0f);
	// Girl's room (east) in the dormer: bed, dolls, shelf; the dormer box juts from the front slope.
	// A fold-up cot in the dormer: a thin mattress on flimsy metal legs (it folded in half if you sat on one side).
	Tinted(ESurface::InteriorWall, FLinearColor(0.9f, 0.55f, 0.75f), 245.0f, 240.0f, Z1 + 32.0f, 320.0f, 410.0f, Z1 + 40.0f);
	for (const float X : { 247.0f, 316.0f })
	{
		for (const float Y : { 242.0f, 406.0f })
		{
			Tinted(ESurface::Stone, FLinearColor(0.5f, 0.5f, 0.52f), X - 2.0f, Y - 2.0f, Z1, X + 2.0f, Y + 2.0f, Z1 + 32.0f);
		}
	}
	Put(EFurniture::Nightstand, HX - 40.0f, -190.0f, 180.0f, Z1);
	Box(ESurface::Stone, 200.0f, -180.0f, Z1, 240.0f, -150.0f, Z1 + 28.0f);
	Put(EFurniture::Shelf, 260.0f, 285.0f, 90.0f, Z1);
	Put(EFurniture::CeilingLamp, 250.0f, 50.0f, 0.0f, Z1 + 200.0f);
	{
		const float DX0 = 130.0f, DX1 = 370.0f, DY0 = 230.0f, DY1 = 420.0f, DZ1 = Z1 + 300.0f;
		Box(ESurface::ExteriorWall, DX0, DY0, Z1, DX0 + 12.0f, DY1, DZ1);
		Box(ESurface::ExteriorWall, DX1 - 12.0f, DY0, Z1, DX1, DY1, DZ1);
		FWall Front; Front.bAlongX = true; Front.Fixed = DY1; Front.From = DX0; Front.To = DX1; Front.Thickness = 12.0f; Front.bExterior = true;
		Front.Openings.Add({ 250.0f, 130.0f, 70.0f, 235.0f, true });
		B.Wall(Front, Z1, DZ1);
		FPiece Roof; Roof.Surface = ESurface::SlateRoof;
		Roof.Center = FVector(250.0f, 325.0f, DZ1 + 8.0f); Roof.Size = FVector(DX1 - DX0 + 60.0f, DY1 - DY0 + 80.0f, 10.0f);
		Roof.Rotation = FRotator(8.0f, 0.0f, 0.0f);
		House.Pieces.Add(Roof);
	}

	// ================= Roof, porch, steps =================
	const float Top = Z1 + Knee;
	if (FParse::Param(FCommandLine::Get(), TEXT("NoRoof")))
	{
		House.Height = Top;
	}
	else
	{
		const float Rise = GableRoofY(0.0f, 0.0f, HX, HY, Top, 44.0f, ESurface::SlateRoof);  // light grey asphalt shingles (tinted in the streamer)
		House.Height = Top + Rise;
		// Upstairs ceilings: the angled parts under the roof slopes are painted the wall's light blue; the flat middle is white.
		const float FlatZ = Z1 + 240.0f;
		const float FlatX = HX - (FlatZ - Top) / FMath::Tan(FMath::DegreesToRadians(44.0f));
		Tinted(ESurface::InteriorWall, FLinearColor(1.5f, 1.5f, 1.5f), -FlatX, -HY + 11.0f, FlatZ, FlatX, HY - 11.0f, FlatZ + 4.0f);
		for (const float Sx : { -1.0f, 1.0f })
		{
			const float Run = HX - FlatX, Lift = FlatZ - Top;
			FPiece Lining;
			Lining.Surface = ESurface::InteriorWall;
			Lining.Center = FVector(Sx * (FlatX + Run * 0.5f), 0.0f, Top + Lift * 0.5f - 7.0f);
			Lining.Size = FVector(HY * 2.0f - 22.0f, FMath::Sqrt(Run * Run + Lift * Lift) + 8.0f, 3.0f);
			Lining.Rotation = FRotator(0.0f, 90.0f, -Sx * 44.0f);
			Lining.bTinted = true;
			Lining.Tint = FLinearColor(0.72f, 0.9f, 1.35f);
			House.Pieces.Add(Lining);
		}
	}
	// Gable-end windows: a pair side by side in the front gable, a small one at the back (drawn just outside the gable siding).
	for (const float X : { -42.0f, 42.0f })
	{
		Tinted(ESurface::ExteriorWall, FLinearColor(2.4f, 2.4f, 2.4f), X - 38.0f, HY + 4.0f, Z1 + 140.0f, X + 38.0f, HY + 16.0f, Z1 + 290.0f);
		Box(ESurface::Glass, X - 30.0f, HY + 14.0f, Z1 + 148.0f, X + 30.0f, HY + 17.0f, Z1 + 282.0f);
	}
	Tinted(ESurface::ExteriorWall, FLinearColor(2.4f, 2.4f, 2.4f), -30.0f, -HY - 16.0f, Z1 + 150.0f, 30.0f, -HY - 4.0f, Z1 + 250.0f);
	Box(ESurface::Glass, -24.0f, -HY - 17.0f, Z1 + 156.0f, 24.0f, -HY - 14.0f, Z1 + 244.0f);
	// Enclosed front porch across the whole front: siding skirt, banks of windows, a lean-to roof, wooden steps to the yard.
	{
		const float PX0 = -HX, PX1 = HX, PY = HY + 220.0f;
		Box(ESurface::Stone, PX0 - 15.0f, HY, -110.0f, PX1 + 15.0f, PY + 15.0f, -8.0f);
		Box(ESurface::PlankFloor, PX0, HY, -8.0f, PX1, PY, 0.0f);
		FWall Front; Front.bAlongX = true; Front.Fixed = PY; Front.From = PX0; Front.To = PX1; Front.Thickness = 14.0f; Front.bExterior = true;
		Front.Openings.Add({ 0.0f, 100.0f, 0.0f, DoorHeight, false });
		// Two wide banks of double windows either side of the door, plus a narrow light next to it.
		for (const float X : { -340.0f, -220.0f, 220.0f, 340.0f })
		{
			Front.Openings.Add({ X, 105.0f, 95.0f, 225.0f, true });
		}
		for (const float X : { -95.0f, 95.0f })
		{
			Front.Openings.Add({ X, 40.0f, 95.0f, 225.0f, true });
		}
		B.Wall(Front, 0.0f, 240.0f);
		FWall WestW; WestW.bAlongX = false; WestW.Fixed = PX0; WestW.From = HY; WestW.To = PY; WestW.Thickness = 14.0f; WestW.bExterior = true;
		WestW.Openings.Add({ HY + 110.0f, 130.0f, 95.0f, 225.0f, true });
		B.Wall(WestW, 0.0f, 240.0f);
		FWall EastW = WestW; EastW.Fixed = PX1;
		B.Wall(EastW, 0.0f, 240.0f);
		FPiece Roof; Roof.Surface = ESurface::SlateRoof;
		Roof.Center = FVector(0.0f, HY + 110.0f, 262.0f);
		Roof.Size = FVector(PX1 - PX0 + 70.0f, 290.0f, 10.0f);
		Roof.Rotation = FRotator(0.0f, 0.0f, 10.0f);
		House.Pieces.Add(Roof);
		// Poured-concrete steps (formed, with a solid slab either side) and a painted-metal pipe rail on the left.
		const FLinearColor Metal(0.32f, 0.33f, 0.34f);
		Box(ESurface::Stone, -65.0f, PY + 15.0f, -110.0f, 65.0f, PY + 15.0f + 140.0f, -110.0f + 4.0f);
		for (const float Sx : { -1.0f, 1.0f })
		{
			Box(ESurface::Stone, Sx * 62.0f - 6.0f, PY + 15.0f, -110.0f, Sx * 62.0f + 6.0f, PY + 155.0f, -50.0f);
		}
		Tinted(ESurface::Stone, Metal, -60.0f, PY + 15.0f, 40.0f, -56.0f, PY + 160.0f, 46.0f);   // rail top
		for (const float Y : { PY + 15.0f, PY + 85.0f, PY + 155.0f })
		{
			Tinted(ESurface::Stone, Metal, -61.0f, Y - 2.0f, -60.0f, -57.0f, Y + 2.0f, 46.0f);   // posts
		}
	}
	// Back door: a small platform at the door with the stairs down from it, joined to the larger deck running east along the wall.
	{
		const FLinearColor Pine(1.7f, 1.35f, 0.8f);
		Tinted(ESurface::Wood, Pine, -310.0f, -HY - 90.0f, -14.0f, -190.0f, -HY, 0.0f);            // platform at the door
		Tinted(ESurface::Wood, Pine, -190.0f, -HY - 170.0f, -14.0f, 150.0f, -HY, 0.0f);            // the deck (3.4 m x 1.7 m as before), level with the platform
		for (int32 k = 0; k < 5; ++k)
		{
			Tinted(ESurface::Wood, Pine, -310.0f, -HY - 90.0f - (k + 1) * 26.0f, -110.0f, -190.0f, -HY - 90.0f - k * 26.0f, -110.0f + (5 - k) * 18.0f + 18.0f);
		}
		Tinted(ESurface::Wood, Pine, -190.0f, -HY - 176.0f, 0.0f, 150.0f, -HY - 170.0f, 95.0f);    // deck rail, outer edge
		Tinted(ESurface::Wood, Pine, 144.0f, -HY - 170.0f, 0.0f, 150.0f, -HY, 95.0f);              // deck rail, east end
	}

	// Interior walls were each a solid colour or wood panelling, never patterned: light blue everywhere upstairs; downstairs
	// (a best guess until confirmed) earth-tone paint, with panelling in the hall, the baby's room and the basement bar side.
	for (FPiece& P : House.Pieces)
	{
		if (P.bFurniture || P.bTinted || P.Surface != ESurface::InteriorWall || FMath::Abs(P.Center.X) > HX || P.Center.Y < -HY || P.Center.Y > HY) continue;
		if (P.Size.Z < 100.0f || P.Size.Z > 320.0f) continue; // only wall slabs
		const float X = P.Center.X, Y = P.Center.Y, Zc = P.Center.Z;
		bool bPanel = false;
		FLinearColor C(1.25f, 1.02f, 0.78f);
		if (Zc < 0.0f)                 { bPanel = X > 0.0f && Y > 0.0f; C = FLinearColor(0.75f, 0.5f, 0.32f); }        // basement: panelled bar wall
		else if (Zc < Storey)          // ground floor
		{
			if (FMath::Abs(X) < 60.0f) { bPanel = true; C = FLinearColor(0.8f, 0.55f, 0.36f); }                            // hall
			else if (Y > 270.0f)       { C = FLinearColor(1.25f, 1.02f, 0.78f); }                                          // living room: tan
			else if (X > 0.0f && Y > -60.0f) { C = FLinearColor(1.45f, 1.2f, 0.62f); }                                     // kitchen: harvest yellow
			else if (X > 0.0f)         { C = FLinearColor(1.15f, 1.0f, 0.8f); }                                             // pantry and bath
			else if (Y > -60.0f)       { C = FLinearColor(0.95f, 1.0f, 0.66f); }                                            // master: olive
			else                       { bPanel = true; C = FLinearColor(0.78f, 0.53f, 0.34f); }                            // baby's room
		}
		else                           // upstairs
		{
			C = FLinearColor(0.72f, 0.9f, 1.35f);                                                                            // every upstairs wall: solid light blue
		}
		if (bPanel) P.Surface = ESurface::Wood;
		P.bTinted = true;
		P.Tint = C;
	}

	// ================= Garage, parking pad, fence =================
	const int32 YardStart = House.Pieces.Num();
	{
		const float GX = 340.0f, GY = -1600.0f, GHX = 300.0f, GHY = 320.0f;
		Box(ESurface::Stone, GX - GHX - 10.0f, GY - GHY - 10.0f, -60.0f, GX + GHX + 10.0f, GY + GHY + 10.0f, 0.0f);
		auto GWall = [&](bool bAlongX, float Fixed, float From, float To, std::initializer_list<FOpening> Openings)
		{
			FWall W;
			W.bAlongX = bAlongX; W.Fixed = Fixed; W.From = From; W.To = To; W.Thickness = OuterWall; W.bExterior = true;
			for (const FOpening& O : Openings) W.Openings.Add(O);
			B.Wall(W, 0.0f, 260.0f);
		};
		GWall(true, GY + GHY, GX - GHX, GX + GHX, { { GX - 120.0f, 90.0f, 0.0f, DoorHeight, false }, { GX + 120.0f, 100.0f, WinB, WinT, true } });
		GWall(true, GY - GHY, GX - GHX, GX + GHX, { { GX, 260.0f, 0.0f, 215.0f, false } });
		GWall(false, GX - GHX, GY - GHY, GY + GHY, { { GY, 100.0f, WinB, WinT, true } });
		GWall(false, GX + GHX, GY - GHY, GY + GHY, {});
		Box(ESurface::Wood, GX - 130.0f, GY - GHY - 12.0f, 0.0f, GX + 130.0f, GY - GHY - 4.0f, 215.0f);
		Box(ESurface::Stone, GX - GHX, GY - GHY, -20.0f, GX + GHX, GY + GHY, 0.0f);
		Box(ESurface::InteriorWall, GX - GHX, GY - GHY, 260.0f, GX + GHX, GY + GHY, 280.0f);
		GableRoof(GX, GY, GHX, GHY, 280.0f, 28.0f, ESurface::SlateRoof);
		Tinted(ESurface::Stone, AsphaltC, GX - 280.0f, GY - GHY - 620.0f, 0.0f, GX + 280.0f, GY - GHY - 20.0f, 3.0f);
		const float FenceX = 780.0f, FenceFront = HY + 520.0f, FenceBack = GY - GHY - 650.0f;
		auto Post = [&](float X, float Y) { Tinted(ESurface::Stone, FLinearColor(0.55f, 0.55f, 0.55f), X - 3.0f, Y - 3.0f, 0.0f, X + 3.0f, Y + 3.0f, 112.0f); };
		auto Mesh = [&](float X0, float Y0, float X1, float Y1) { Tinted(ESurface::Glass, FLinearColor(0.7f, 0.7f, 0.7f), X0, Y0, 5.0f, X1, Y1, 105.0f); };
		auto Rails = [&](float X0, float Y0, float X1, float Y1)
		{
			for (const float Z : { 8.0f, 55.0f, 108.0f }) Tinted(ESurface::Stone, FLinearColor(0.6f, 0.6f, 0.62f), X0, Y0, Z, X1, Y1, Z + 2.5f);
		};
		Rails(-FenceX, FenceBack, -FenceX + 1.5f, FenceFront);
		Rails(FenceX - 1.5f, FenceBack, FenceX, FenceFront);
		Rails(-FenceX, FenceBack, FenceX, FenceBack + 1.5f);
		Rails(-FenceX, FenceFront - 1.5f, -150.0f, FenceFront);
		Rails(150.0f, FenceFront - 1.5f, FenceX, FenceFront);
		for (float Y = FenceBack; Y <= FenceFront; Y += 300.0f)
		{
			Post(-FenceX, Y); Post(FenceX, Y);
			Mesh(-FenceX - 0.6f, Y, -FenceX + 0.6f, Y + 300.0f);
			Mesh(FenceX - 0.6f, Y, FenceX + 0.6f, Y + 300.0f);
		}
		for (float X = -FenceX; X < FenceX; X += 300.0f)
		{
			Post(X, FenceBack);
			Mesh(X, FenceBack - 0.6f, FMath::Min(X + 300.0f, FenceX), FenceBack + 0.6f);
			if (X > -150.0f && X < 150.0f) continue;
			Post(X, FenceFront);
			Mesh(X, FenceFront - 0.6f, FMath::Min(X + 300.0f, FenceX), FenceFront + 0.6f);
		}
	}
	for (int32 k = YardStart; k < House.Pieces.Num(); ++k) House.Pieces[k].Center.Z -= 100.0f;
	PruneFurniture(House); return House;
}

FHouse GenerateChurch(uint32 Seed, EStyle Style)
{
	FHouse House;
	FRand Rand{ Seed };
	const float W = FMath::GridSnap(Rand.Range(950.0f, 1150.0f), 10.0f);
	const float D = FMath::GridSnap(Rand.Range(2000.0f, 2400.0f), 10.0f);
	const float WallH = 780.0f;
	const float HX = W * 0.5f, HY = D * 0.5f;
	House.Width = W;
	House.Depth = D;
	FBuilder B{ House, Style == EStyle::Brick ? ESurface::BrickWall : Style == EStyle::Timber ? ESurface::TimberWall : ESurface::ExteriorWall };

	B.Box(ESurface::Stone, FVector(-HX - 25.0f, -HY - 25.0f, -60.0f), FVector(HX + 25.0f, HY + 25.0f, 0.0f));
	B.Box(ESurface::TileFloor, FVector(-HX, -HY, -20.0f), FVector(HX, HY, 0.0f));

	// Nave walls: tall windows down both sides, one high window behind the altar.
	for (int32 Side = 0; Side < 4; ++Side)
	{
		FWall Wall;
		Wall.bAlongX = Side >= 2;
		Wall.Fixed = Side == 0 ? -HX : Side == 1 ? HX : Side == 2 ? -HY : HY;
		Wall.From = Wall.bAlongX ? -HX : -HY;
		Wall.To = Wall.bAlongX ? HX : HY;
		Wall.Thickness = OuterWall + 10.0f;
		Wall.bExterior = true;
		if (Side < 2)
		{
			const int32 Count = 5;
			for (int32 k = 0; k < Count; ++k)
			{
				const float At = FMath::Lerp(-HY + 350.0f, HY - 350.0f, (k + 0.5f) / Count);
				Wall.Openings.Add({ At, 120.0f, 200.0f, 600.0f, true });
			}
		}
		else if (Side == 3)
		{
			Wall.Openings.Add({ 0.0f, 180.0f, 0.0f, 340.0f, false });
		}
		else
		{
			Wall.Openings.Add({ 0.0f, 140.0f, 620.0f, 740.0f, true });
		}
		B.Wall(Wall, 0.0f, WallH);
	}

	// Bell tower over the entrance: three more walls making a vestibule, then a stepped spire.
	const float TW = 240.0f, TD = 480.0f, TowerH = 1500.0f;
	{
		FWall Front;
		Front.bAlongX = true; Front.Fixed = HY + TD; Front.From = -TW; Front.To = TW; Front.Thickness = OuterWall; Front.bExterior = true;
		Front.Openings.Add({ 0.0f, 180.0f, 0.0f, 340.0f, false });
		FWall Left;
		Left.bAlongX = false; Left.Fixed = -TW; Left.From = HY; Left.To = HY + TD; Left.Thickness = OuterWall; Left.bExterior = true;
		FWall Right = Left;
		Right.Fixed = TW;
		B.Wall(Front, 0.0f, TowerH);
		B.Wall(Left, 0.0f, TowerH);
		B.Wall(Right, 0.0f, TowerH);
		B.Box(ESurface::TileFloor, FVector(-TW, HY, -20.0f), FVector(TW, HY + TD, 0.0f));
		B.Box(ESurface::Stone, FVector(-TW - 25.0f, HY, -60.0f), FVector(TW + 25.0f, HY + TD + 25.0f, -20.0f));
		B.Box(ESurface::InteriorWall, FVector(-TW, HY, WallH), FVector(TW, HY + TD, WallH + SlabThickness));
		const int32 Steps = 16;
		for (int32 i = 0; i < Steps; ++i)
		{
			const float T = 1.0f - static_cast<float>(i) / Steps;
			const float Z0 = TowerH + i * 90.0f;
			B.Box(ESurface::SlateRoof, FVector(-TW * T - 20.0f, HY + TD * 0.5f - TD * 0.5f * T - 20.0f, Z0), FVector(TW * T + 20.0f, HY + TD * 0.5f + TD * 0.5f * T + 20.0f, Z0 + 92.0f));
		}
	}

	// Pews in two banks either side of a central aisle, facing the altar at the -Y end.
	const float Aisle = 200.0f;
	for (int32 Row = 0; Row < 8; ++Row)
	{
		const float Y = -HY + 700.0f + Row * 150.0f;
		for (const float Sx : { -1.0f, 1.0f })
		{
			const float X0 = Sx * (Aisle * 0.5f + 20.0f);
			const float X1 = Sx * (HX - 60.0f);
			const float Lo = FMath::Min(X0, X1), Hi = FMath::Max(X0, X1);
			B.Box(ESurface::Wood, FVector(Lo, Y, 42.0f), FVector(Hi, Y + 45.0f, 50.0f));
			B.Box(ESurface::Wood, FVector(Lo, Y + 40.0f, 50.0f), FVector(Hi, Y + 48.0f, 105.0f));
			B.Box(ESurface::Wood, FVector(Lo, Y + 5.0f, 0.0f), FVector(Lo + 8.0f, Y + 40.0f, 42.0f));
			B.Box(ESurface::Wood, FVector(Hi - 8.0f, Y + 5.0f, 0.0f), FVector(Hi, Y + 40.0f, 42.0f));
		}
	}
	// Chancel: raised step, altar, lectern and a cross on the end wall.
	B.Box(ESurface::Stone, FVector(-HX + 12.0f, -HY + 12.0f, 0.0f), FVector(HX - 12.0f, -HY + 480.0f, 25.0f));
	B.Box(ESurface::Stone, FVector(-120.0f, -HY + 100.0f, 25.0f), FVector(120.0f, -HY + 190.0f, 115.0f));
	B.Box(ESurface::Wood, FVector(-14.0f, -HY + 14.0f, 300.0f), FVector(14.0f, -HY + 22.0f, 560.0f));
	B.Box(ESurface::Wood, FVector(-90.0f, -HY + 14.0f, 450.0f), FVector(90.0f, -HY + 22.0f, 478.0f));
	B.Box(ESurface::Wood, FVector(HX - 150.0f, -HY + 260.0f, 25.0f), FVector(HX - 90.0f, -HY + 320.0f, 130.0f));

	// Buttresses between the windows, cornice bands, pointed stone heads over the windows and belfry louvres.
	for (const float Sx : { -1.0f, 1.0f })
	{
		for (int32 k = 0; k <= 5; ++k)
		{
			const float Y = FMath::Lerp(-HY + 350.0f, HY - 350.0f, k / 5.0f) - 0.5f * (HY * 2.0f - 700.0f) / 5.0f;
			if (Y < -HY + 40.0f || Y > HY - 40.0f) continue;
			B.Box(ESurface::Stone, FVector(FMath::Min(Sx * (HX + 8.0f), Sx * (HX + 62.0f)), Y - 22.0f, 0.0f), FVector(FMath::Max(Sx * (HX + 8.0f), Sx * (HX + 62.0f)), Y + 22.0f, 470.0f));
			B.Box(ESurface::Stone, FVector(FMath::Min(Sx * (HX + 8.0f), Sx * (HX + 40.0f)), Y - 18.0f, 470.0f), FVector(FMath::Max(Sx * (HX + 8.0f), Sx * (HX + 40.0f)), Y + 18.0f, 700.0f));
		}
		for (int32 k = 0; k < 5; ++k)
		{
			const float At = FMath::Lerp(-HY + 350.0f, HY - 350.0f, (k + 0.5f) / 5.0f);
			const float Widths[] = { 150.0f, 110.0f, 70.0f, 30.0f };
			for (int32 j = 0; j < 4; ++j)
			{
				B.Box(ESurface::Stone, FVector(FMath::Min(Sx * (HX + 8.0f), Sx * (HX + 22.0f)), At - Widths[j] * 0.5f, 600.0f + j * 28.0f), FVector(FMath::Max(Sx * (HX + 8.0f), Sx * (HX + 22.0f)), At + Widths[j] * 0.5f, 628.0f + j * 28.0f));
			}
		}
	}
	for (const float Sy : { -1.0f, 1.0f })
	{
		B.Box(ESurface::Stone, FVector(-HX - 30.0f, FMath::Min(Sy * (HY + 8.0f), Sy * (HY + 30.0f)), WallH - 30.0f), FVector(HX + 30.0f, FMath::Max(Sy * (HY + 8.0f), Sy * (HY + 30.0f)), WallH));
	}
	for (const float Sx : { -1.0f, 1.0f })
	{
		B.Box(ESurface::Stone, FVector(FMath::Min(Sx * (HX + 8.0f), Sx * (HX + 30.0f)), -HY - 30.0f, WallH - 30.0f), FVector(FMath::Max(Sx * (HX + 8.0f), Sx * (HX + 30.0f)), HY + 30.0f, WallH));
		B.Box(ESurface::Wood, FVector(Sx * (TW + 6.0f) - 6.0f, HY + TD * 0.5f - 50.0f, 1050.0f), FVector(Sx * (TW + 6.0f) + 6.0f, HY + TD * 0.5f + 50.0f, 1380.0f));
	}
	B.Box(ESurface::Wood, FVector(-50.0f, HY + TD + 6.0f - 6.0f, 1050.0f), FVector(50.0f, HY + TD + 6.0f + 6.0f, 1380.0f));
	B.Box(ESurface::Stone, FVector(-TW - 30.0f, HY + TD, 780.0f), FVector(TW + 30.0f, HY + TD + 30.0f, 810.0f));
	// Stepped cornice under the tower's belfry.
	B.Box(ESurface::Stone, FVector(-TW - 25.0f, HY - 10.0f, TowerH - 30.0f), FVector(TW + 25.0f, HY + TD + 25.0f, TowerH));
	// Pitched roof, ridge along the nave, gables at both ends.
	const float Pitch = 40.0f;
	const float RiseH = HX * FMath::Tan(FMath::DegreesToRadians(Pitch));
	const int32 Courses = 30;
	for (int32 c = 0; c < Courses; ++c)
	{
		const float Z0 = WallH + c * RiseH / Courses;
		const float Half = HX * (1.0f - (c + 0.5f) / Courses);
		for (const float Sy : { -1.0f, 1.0f })
		{
			B.Box(B.OuterSurface, FVector(-Half, Sy * HY - OuterWall * 0.5f, Z0), FVector(Half, Sy * HY + OuterWall * 0.5f, Z0 + RiseH / Courses));
		}
	}
	const float Slope = FMath::Sqrt(HX * HX + RiseH * RiseH) + 60.0f;
	for (const float Sx : { -1.0f, 1.0f })
	{
		FPiece P;
		P.Surface = ESurface::SlateRoof;
		P.Center = FVector(Sx * HX * 0.5f, 0.0f, WallH + RiseH * 0.5f + 12.0f);
		P.Size = FVector(D + 80.0f, Slope, 14.0f);
		P.Rotation = FRotator(0.0f, 90.0f, -Sx * Pitch);
		House.Pieces.Add(P);
	}
	House.Height = TowerH + 16 * 90.0f;
	return House;
}
}
