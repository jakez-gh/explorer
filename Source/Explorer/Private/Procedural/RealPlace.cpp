#include "Procedural/RealPlace.h"
#include <atomic>
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	// Where the place sits in the world (X north, Y east), far from the start, on the main continent.
	const FVector2D WorldOrigin(2500000.0, 1500000.0);
	constexpr double BlendMargin = 400000.0; // 4 km of terrain easing back to the procedural world
	constexpr float BaseElevation = 294.0f;  // metres above sea level that maps to world Z = 0
	constexpr double RasterCell = 2000.0;    // cm
	constexpr double RoadBucket = 50000.0;   // cm

	enum ECover : uint8
	{
		CoverField = 0, CoverResidential, CoverCommercial, CoverPark, CoverWood, CoverWater, CoverCemetery,
		CoverMask = 0x0F,
		CoverBuilding = 0x80,
	};

	struct FSegment
	{
		FVector2D A, B;
		float Width;
		bool bRoad;
		uint32 Seed;
		int32 Way = -1;
	};

	struct FWay
	{
		FString Name;
		TArray<FVector2D> Pts;
		float Width;
	};

	struct FData
	{
		bool bLoaded = false;
		// World cm bounds of the baked data.
		FVector2D Min, Max;
		int32 Rows = 0, Cols = 0;
		TArray<float> Elevation;
		TArray<FSegment> Segments;
		TMap<FIntPoint, TArray<int32>> RoadGrid;
		TArray<RealPlace::FBuilding> Buildings;
		TArray<FWay> Ways;
		TArray<RealPlace::FLot> Lots;
		TMap<FIntPoint, TArray<int32>> LotGrid;
		TMap<FIntPoint, TArray<int32>> BuildingGrid;
		int32 RasterW = 0, RasterH = 0;
		TArray<uint8> Raster;
		TArray<RealPlace::FLandmark> Landmarks;
		TArray<TPair<TArray<FVector2D>, FString>> SchoolAreas;
	};

	FData Place;
	std::atomic<bool> bReady{ false };

	FVector2D ToWorld(double East, double North) { return FVector2D(WorldOrigin.X + North * 100.0, WorldOrigin.Y + East * 100.0); }

	float Smooth(float T) { T = FMath::Clamp(T, 0.0f, 1.0f); return T * T * (3.0f - 2.0f * T); }

	void ReadPoints(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, TArray<FVector2D>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(Field, Arr))
		{
			return;
		}
		for (int32 i = 0; i + 1 < Arr->Num(); i += 2)
		{
			Out.Add(ToWorld((*Arr)[i]->AsNumber(), (*Arr)[i + 1]->AsNumber()));
		}
	}

	// Fill a polygon into the raster (even-odd, scanline per cell row).
	void FillPolygon(const TArray<FVector2D>& Poly, TFunctionRef<void(uint8&)> Set)
	{
		if (Poly.Num() < 3)
		{
			return;
		}
		double MinX = TNumericLimits<double>::Max(), MaxX = -MinX;
		for (const FVector2D& P : Poly)
		{
			MinX = FMath::Min(MinX, P.X);
			MaxX = FMath::Max(MaxX, P.X);
		}
		const int32 X0 = FMath::Clamp(FMath::FloorToInt((MinX - Place.Min.X) / RasterCell), 0, Place.RasterH - 1);
		const int32 X1 = FMath::Clamp(FMath::FloorToInt((MaxX - Place.Min.X) / RasterCell), 0, Place.RasterH - 1);
		TArray<double> Crossings;
		for (int32 RowX = X0; RowX <= X1; ++RowX)
		{
			// Raster rows run along world X (north); columns along world Y (east).
			const double X = Place.Min.X + (RowX + 0.5) * RasterCell;
			Crossings.Reset();
			for (int32 i = 0; i < Poly.Num(); ++i)
			{
				const FVector2D& A = Poly[i];
				const FVector2D& B = Poly[(i + 1) % Poly.Num()];
				if ((A.X <= X && B.X > X) || (B.X <= X && A.X > X))
				{
					Crossings.Add(A.Y + (X - A.X) / (B.X - A.X) * (B.Y - A.Y));
				}
			}
			Crossings.Sort();
			for (int32 c = 0; c + 1 < Crossings.Num(); c += 2)
			{
				const int32 C0 = FMath::Clamp(FMath::CeilToInt((Crossings[c] - Place.Min.Y) / RasterCell - 0.5), 0, Place.RasterW - 1);
				const int32 C1 = FMath::Clamp(FMath::FloorToInt((Crossings[c + 1] - Place.Min.Y) / RasterCell - 0.5), 0, Place.RasterW - 1);
				for (int32 Col = C0; Col <= C1; ++Col)
				{
					Set(Place.Raster[RowX * Place.RasterW + Col]);
				}
			}
		}
	}

	uint8 RasterAt(int32 RowX, int32 Col)
	{
		if (RowX < 0 || Col < 0 || RowX >= Place.RasterH || Col >= Place.RasterW)
		{
			return CoverField;
		}
		return Place.Raster[RowX * Place.RasterW + Col];
	}

	// Bilinear 0..1 water fraction so banks slope instead of stepping.
	float WaterFraction(double X, double Y)
	{
		const double FX = (X - Place.Min.X) / RasterCell - 0.5;
		const double FY = (Y - Place.Min.Y) / RasterCell - 0.5;
		const int32 IX = FMath::FloorToInt(FX), IY = FMath::FloorToInt(FY);
		const float TX = FX - IX, TY = FY - IY;
		auto W = [](uint8 V) { return (V & CoverMask) == CoverWater ? 1.0f : 0.0f; };
		return FMath::Lerp(FMath::Lerp(W(RasterAt(IX, IY)), W(RasterAt(IX, IY + 1)), TY),
			FMath::Lerp(W(RasterAt(IX + 1, IY)), W(RasterAt(IX + 1, IY + 1)), TY), TX);
	}

	float ElevationAt(double X, double Y)
	{
		// The DEM grid spans the data bounds; rows run south to north (world X), columns west to east (world Y).
		const double U = FMath::Clamp((X - Place.Min.X) / (Place.Max.X - Place.Min.X), 0.0, 1.0) * (Place.Rows - 1);
		const double V = FMath::Clamp((Y - Place.Min.Y) / (Place.Max.Y - Place.Min.Y), 0.0, 1.0) * (Place.Cols - 1);
		const int32 R0 = FMath::Min(FMath::FloorToInt(U), Place.Rows - 2), C0 = FMath::Min(FMath::FloorToInt(V), Place.Cols - 2);
		const float TR = Smooth(U - R0), TC = Smooth(V - C0);
		auto E = [&](int32 R, int32 C) { return Place.Elevation[R * Place.Cols + C]; };
		return FMath::Lerp(FMath::Lerp(E(R0, C0), E(R0, C0 + 1), TC), FMath::Lerp(E(R0 + 1, C0), E(R0 + 1, C0 + 1), TC), TR);
	}

	float RoadWidth(const FString& Class, bool& bRoad, bool& bKeep)
	{
		bRoad = true;
		bKeep = true;
		if (Class == TEXT("motorway") || Class == TEXT("trunk")) return 1600.0f;
		if (Class == TEXT("motorway_link") || Class == TEXT("trunk_link")) return 900.0f;
		if (Class == TEXT("primary")) return 1200.0f;
		if (Class == TEXT("secondary") || Class == TEXT("primary_link")) return 1000.0f;
		if (Class == TEXT("tertiary") || Class == TEXT("secondary_link") || Class == TEXT("tertiary_link")) return 850.0f;
		if (Class == TEXT("residential") || Class == TEXT("unclassified") || Class == TEXT("living_street")) return 700.0f;
		if (Class == TEXT("service")) return 400.0f;
		bRoad = false;
		if (Class == TEXT("track")) return 320.0f;
		if (Class == TEXT("path") || Class == TEXT("bridleway") || Class == TEXT("cycleway")) return 160.0f;
		bKeep = false; // footways, steps and the like
		return 0.0f;
	}
}

namespace RealPlace
{
	FVector2D Origin() { return WorldOrigin; }
	bool IsLoaded() { return bReady.load(); }
	FVector2D FromLocal(double EastM, double NorthM) { return ToWorld(EastM, NorthM); }
	const TArray<FLandmark>& Landmarks() { return Place.Landmarks; }

	bool FindLandmark(const FString& NameContains, FVector2D& OutPos)
	{
		for (const FLandmark& L : Place.Landmarks)
		{
			if (L.Name.Contains(NameContains))
			{
				OutPos = L.Pos;
				return true;
			}
		}
		return false;
	}

	bool Load()
	{
		if (bReady.load())
		{
			return true;
		}
		FString Text;
		const FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("Data/CouncilBluffs.json"));
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			UE_LOG(LogTemp, Warning, TEXT("RealPlace: %s not found; run Tools/fetch_council_bluffs.py"), *Path);
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("RealPlace: could not parse %s"), *Path);
			return false;
		}
		Text.Empty();

		const TSharedPtr<FJsonObject> Bounds = Root->GetObjectField(TEXT("bounds"));
		Place.Min = ToWorld(Bounds->GetNumberField(TEXT("west")), Bounds->GetNumberField(TEXT("south")));
		Place.Max = ToWorld(Bounds->GetNumberField(TEXT("east")), Bounds->GetNumberField(TEXT("north")));
		// World X is north, Y is east: reorder so Min < Max on both axes.
		{
			const FVector2D A = Place.Min, B = Place.Max;
			Place.Min = FVector2D(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y));
			Place.Max = FVector2D(FMath::Max(A.X, B.X), FMath::Max(A.Y, B.Y));
		}
		Place.RasterH = FMath::CeilToInt((Place.Max.X - Place.Min.X) / RasterCell);
		Place.RasterW = FMath::CeilToInt((Place.Max.Y - Place.Min.Y) / RasterCell);
		Place.Raster.Init(CoverField, Place.RasterH * Place.RasterW);

		const TSharedPtr<FJsonObject> Elev = Root->GetObjectField(TEXT("elevation"));
		Place.Rows = Elev->GetIntegerField(TEXT("rows"));
		Place.Cols = Elev->GetIntegerField(TEXT("cols"));
		for (const TSharedPtr<FJsonValue>& V : Elev->GetArrayField(TEXT("m")))
		{
			Place.Elevation.Add(V->AsNumber());
		}

		// Land cover, painted in priority order: fields, then built-up land, parks, woods, then water on top.
		struct FArea { TArray<FVector2D> Poly; uint8 Cover; FString Name, Kind; };
		TArray<FArea> Areas;
		for (const TSharedPtr<FJsonValue>& V : Root->GetArrayField(TEXT("areas")))
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			const FString Kind = O->GetStringField(TEXT("k"));
			uint8 Cover = 255;
			if (Kind == TEXT("residential")) Cover = CoverResidential;
			else if (Kind == TEXT("commercial") || Kind == TEXT("industrial") || Kind == TEXT("retail") || Kind == TEXT("school") || Kind == TEXT("hospital")) Cover = CoverCommercial;
			else if (Kind == TEXT("park") || Kind == TEXT("recreation_ground") || Kind == TEXT("golf_course") || Kind == TEXT("pitch") || Kind == TEXT("grass") || Kind == TEXT("meadow") || Kind == TEXT("playground") || Kind == TEXT("track") || Kind == TEXT("stadium")) Cover = CoverPark;
			else if (Kind == TEXT("forest") || Kind == TEXT("wood")) Cover = CoverWood;
			else if (Kind == TEXT("cemetery")) Cover = CoverCemetery;
			FArea A;
			A.Cover = Cover;
			A.Name = O->GetStringField(TEXT("n"));
			A.Kind = Kind;
			ReadPoints(O, TEXT("p"), A.Poly);
			if (A.Poly.Num() >= 3)
			{
				if (!A.Name.IsEmpty())
				{
					FVector2D C = FVector2D::ZeroVector;
					for (const FVector2D& P : A.Poly) C += P;
					Place.Landmarks.Add({ A.Name, C / A.Poly.Num() });
				}
				if (Kind == TEXT("school"))
				{
					Place.SchoolAreas.Add({ A.Poly, A.Name });
				}
				if (Cover != 255)
				{
					Areas.Add(MoveTemp(A));
				}
			}
		}
		for (const uint8 Pass : { CoverResidential, CoverCommercial, CoverCemetery, CoverPark, CoverWood })
		{
			for (const FArea& A : Areas)
			{
				if (A.Cover == Pass)
				{
					FillPolygon(A.Poly, [&](uint8& Cell) { Cell = (Cell & ~CoverMask) | A.Cover; });
				}
			}
		}
		for (const TSharedPtr<FJsonValue>& V : Root->GetArrayField(TEXT("water")))
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (O->GetIntegerField(TEXT("river")) == 0)
			{
				TArray<FVector2D> Poly;
				ReadPoints(O, TEXT("p"), Poly);
				FillPolygon(Poly, [](uint8& Cell) { Cell = (Cell & ~CoverMask) | CoverWater; });
			}
		}

		// Buildings, indexed by 500 m buckets on their centroid.
		for (const TSharedPtr<FJsonValue>& V : Root->GetArrayField(TEXT("buildings")))
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			RealPlace::FBuilding B;
			ReadPoints(O, TEXT("p"), B.Outline);
			if (B.Outline.Num() > 1 && B.Outline[0].Equals(B.Outline.Last(), 1.0))
			{
				B.Outline.Pop();
			}
			if (B.Outline.Num() < 3)
			{
				continue;
			}
			for (const FVector2D& P : B.Outline) B.Centroid += P;
			B.Centroid /= B.Outline.Num();
			B.HeightM = O->GetNumberField(TEXT("h"));
			B.Type = O->GetStringField(TEXT("t"));
			B.Name = O->GetStringField(TEXT("n"));
			B.Address = O->GetStringField(TEXT("a"));
			// A building standing inside a school's grounds is a school building (the mapped campus of the school it belongs to).
			if (B.Type != TEXT("school") && B.Outline.Num() > 0)
			{
				FVector2D C = FVector2D::ZeroVector;
				for (const FVector2D& P : B.Outline) C += P;
				C /= B.Outline.Num();
				for (const TPair<TArray<FVector2D>, FString>& SA : Place.SchoolAreas)
				{
					bool bIn = false;
					for (int32 i = 0, j = SA.Key.Num() - 1; i < SA.Key.Num(); j = i++)
					{
						const FVector2D& A = SA.Key[i];
						const FVector2D& D = SA.Key[j];
						if (((A.X > C.X) != (D.X > C.X)) && (C.Y < (D.Y - A.Y) * (C.X - A.X) / (D.X - A.X) + A.Y)) bIn = !bIn;
					}
					if (bIn)
					{
						B.Type = TEXT("school");
						if (B.Name.IsEmpty()) B.Name = SA.Value;
						break;
					}
				}
			}
			FillPolygon(B.Outline, [](uint8& Cell) { Cell |= CoverBuilding; });
			if (!B.Name.IsEmpty())
			{
				Place.Landmarks.Add({ B.Name, B.Centroid });
			}
			const int32 Index = Place.Buildings.Add(MoveTemp(B));
			const FVector2D C = Place.Buildings[Index].Centroid;
			Place.BuildingGrid.FindOrAdd(FIntPoint(FMath::FloorToInt(C.X / RoadBucket), FMath::FloorToInt(C.Y / RoadBucket))).Add(Index);
		}

		// Roads and trails become straight segments between the mapped points.
		for (const TSharedPtr<FJsonValue>& V : Root->GetArrayField(TEXT("roads")))
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			bool bRoad, bKeep;
			const float Width = RoadWidth(O->GetStringField(TEXT("c")), bRoad, bKeep);
			if (!bKeep)
			{
				continue;
			}
			TArray<FVector2D> Pts;
			ReadPoints(O, TEXT("p"), Pts);
			const int32 WayIndex = Place.Ways.Add({ O->GetStringField(TEXT("n")), Pts, Width });
			for (int32 i = 0; i + 1 < Pts.Num(); ++i)
			{
				if (FVector2D::DistSquared(Pts[i], Pts[i + 1]) < 100.0)
				{
					continue;
				}
				const int32 Index = Place.Segments.Add({ Pts[i], Pts[i + 1], Width, bRoad, static_cast<uint32>(Place.Segments.Num()) });
				Place.Segments[Index].Way = WayIndex;
				const FVector2D Lo(FMath::Min(Pts[i].X, Pts[i + 1].X), FMath::Min(Pts[i].Y, Pts[i + 1].Y));
				const FVector2D Hi(FMath::Max(Pts[i].X, Pts[i + 1].X), FMath::Max(Pts[i].Y, Pts[i + 1].Y));
				for (int32 BY = FMath::FloorToInt(Lo.Y / RoadBucket); BY <= FMath::FloorToInt(Hi.Y / RoadBucket); ++BY)
				{
					for (int32 BX = FMath::FloorToInt(Lo.X / RoadBucket); BX <= FMath::FloorToInt(Hi.X / RoadBucket); ++BX)
					{
						Place.RoadGrid.FindOrAdd(FIntPoint(BX, BY)).Add(Index);
					}
				}
			}
		}

		// Fill neighbourhoods: a house lot every ~15 m on both sides of residential streets, wherever the
		// map shows residential land with no building and no other street close by.
		{
			TArray<FVector2D> AveE; // lots along Avenue E, for locating 1733
			for (int32 WayIndex = 0; WayIndex < Place.Ways.Num(); ++WayIndex)
			{
				const FWay& Way = Place.Ways[WayIndex];
				if (!(FMath::IsNearlyEqual(Way.Width, 700.0f) || FMath::IsNearlyEqual(Way.Width, 850.0f)))
				{
					continue;
				}
				double Carry = 0.0;
				for (int32 i = 0; i + 1 < Way.Pts.Num(); ++i)
				{
					const FVector2D A = Way.Pts[i], B = Way.Pts[i + 1];
					const double Len = FVector2D::Distance(A, B);
					if (Len < 1.0)
					{
						continue;
					}
					const FVector2D T = (B - A) / Len;
					for (double S = Carry; S < Len; S += 1500.0)
					{
						const FVector2D P = A + T * S;
						for (const int32 Side : { -1, 1 })
						{
							const FVector2D N = FVector2D(-T.Y, T.X) * Side;
							const uint32 Seed = static_cast<uint32>(FMath::RoundToInt(P.X / 50.0) * 73856093) ^ static_cast<uint32>(FMath::RoundToInt(P.Y / 50.0) * 19349663) ^ static_cast<uint32>(Side + 3);
							const float RW = 900.0f + (Seed % 300), RD = 800.0f + ((Seed >> 8) % 200);
							const FVector2D C = P + N * (Way.Width * 0.5 + 300.0 + RD * 0.5 + 900.0);
							// Corners must sit on residential land, clear of buildings and water.
							bool bOk = true;
							for (const FVector2D& Off : { FVector2D(0, 0), FVector2D(RW * 0.5, RD * 0.5), FVector2D(-RW * 0.5, RD * 0.5), FVector2D(RW * 0.5, -RD * 0.5), FVector2D(-RW * 0.5, -RD * 0.5) })
							{
								const FVector2D Q = C + T * Off.X + N * Off.Y;
								const int32 RX = FMath::FloorToInt((Q.X - Place.Min.X) / RasterCell), RY = FMath::FloorToInt((Q.Y - Place.Min.Y) / RasterCell);
								if (RX < 0 || RY < 0 || RX >= Place.RasterH || RY >= Place.RasterW)
								{
									bOk = false;
									break;
								}
								const uint8 Cell = Place.Raster[RX * Place.RasterW + RY];
								if (((Cell & CoverMask) != CoverResidential && (Cell & CoverMask) != CoverField) || (Cell & CoverBuilding))
								{
									bOk = false;
									break;
								}
							}
							if (!bOk)
							{
								continue;
							}
							// No other street nearby (own street excepted), and no lot already here.
							for (int32 BY = FMath::FloorToInt((C.Y - 2500.0) / RoadBucket); bOk && BY <= FMath::FloorToInt((C.Y + 2500.0) / RoadBucket); ++BY)
							{
								for (int32 BX = FMath::FloorToInt((C.X - 2500.0) / RoadBucket); bOk && BX <= FMath::FloorToInt((C.X + 2500.0) / RoadBucket); ++BX)
								{
									const TArray<int32>* Bucket = Place.RoadGrid.Find(FIntPoint(BX, BY));
									for (int32 k = 0; Bucket && bOk && k < Bucket->Num(); ++k)
									{
										const FSegment& Seg = Place.Segments[(*Bucket)[k]];
										if (Seg.Way == WayIndex)
										{
											continue;
										}
										const FVector2D AB = Seg.B - Seg.A;
										const double U = FMath::Clamp(FVector2D::DotProduct(C - Seg.A, AB) / FMath::Max(AB.SizeSquared(), 1.0), 0.0, 1.0);
										bOk = FVector2D::Distance(C, Seg.A + AB * U) > Seg.Width * 0.5 + FMath::Max(RW, RD) * 0.5 + 300.0;
									}
								}
							}
							for (int32 BY = FMath::FloorToInt((C.Y - 1200.0) / RoadBucket); bOk && BY <= FMath::FloorToInt((C.Y + 1200.0) / RoadBucket); ++BY)
							{
								for (int32 BX = FMath::FloorToInt((C.X - 1200.0) / RoadBucket); bOk && BX <= FMath::FloorToInt((C.X + 1200.0) / RoadBucket); ++BX)
								{
									const TArray<int32>* Bucket = Place.LotGrid.Find(FIntPoint(BX, BY));
									for (int32 k = 0; Bucket && bOk && k < Bucket->Num(); ++k)
									{
										bOk = FVector2D::Distance(C, Place.Lots[(*Bucket)[k]].Pos) > 1150.0;
									}
								}
							}
							if (!bOk)
							{
								continue;
							}
							// The front (+Y in the house's frame) faces the street: (-sin yaw, cos yaw) = -N.
							RealPlace::FLot Lot;
							Lot.Pos = C;
							Lot.Yaw = FMath::RadiansToDegrees(FMath::Atan2(N.X, -N.Y));
							Lot.Width = RW;
							Lot.Depth = RD;
							Lot.Seed = Seed;
							const int32 LotIndex = Place.Lots.Add(Lot);
							Place.LotGrid.FindOrAdd(FIntPoint(FMath::FloorToInt(C.X / RoadBucket), FMath::FloorToInt(C.Y / RoadBucket))).Add(LotIndex);
							if (Way.Name == TEXT("Avenue E") && Side == -1)
							{
								AveE.Add(C);
							}
						}
					}
					Carry = FMath::Fmod(Carry + 1500.0 - Len, 1500.0);
					if (Carry < 0.0) Carry += 1500.0;
				}
			}
			// 1733 Avenue E: 17th Street is 1,456 m west of Bayliss Park, 18th 124 m further west; house
			// numbers rise westward, so 1733 is the 17th odd-numbered lot after 17th Street.
			const double East17 = ToWorld(-1456.0, 0.0).Y, East18 = ToWorld(-1580.0, 0.0).Y;
			TArray<FVector2D> Block;
			for (const FVector2D& L : AveE)
			{
				if (L.Y <= East17 && L.Y >= East18) Block.Add(L);
			}
			Block.Sort([](const FVector2D& A, const FVector2D& B) { return A.Y > B.Y; });
			UE_LOG(LogTemp, Display, TEXT("RealPlace: %d house lots; %d on Avenue E between 17th and 18th"), Place.Lots.Num(), Block.Num());
			const FVector2D Spot33 = Block.Num() > 0 ? Block[FMath::Min(16, Block.Num() - 1)] : ToWorld(-1497.0, 745.0);
			Place.Landmarks.Add({ TEXT("1733 Avenue E"), Spot33 });
			// The family's house is 1719 Avenue E (renumbered since): the 10th odd number after 17th Street.
			// Zillow's photos of 1733 E Ave match the family's house (grey-blue vinyl, brown trim, enclosed porch, chain-link fence):
			// it is the same house, renumbered, so both addresses lead to it.
			const FVector2D Spot = Spot33;
			Place.Landmarks.Add({ TEXT("1719 Avenue E"), Spot });
			// That lot becomes the hand-planned house.
			int32 Nearest = INDEX_NONE;
			double NearestD = 1500.0;
			for (int32 k = 0; k < Place.Lots.Num(); ++k)
			{
				const double Dist = FVector2D::Distance(Place.Lots[k].Pos, Spot);
				if (Dist < NearestD)
				{
					NearestD = Dist;
					Nearest = k;
				}
			}
			if (Nearest != INDEX_NONE)
			{
				Place.Lots[Nearest].bFamilyHouse = true;
				// Test spots inside the house for -StartAt: local (x, y) in cm from the middle of the ground floor.
				const FLot& Hero = Place.Lots[Nearest];
				auto InHouse = [&](float X, float Y) { const FVector W = FRotator(0.0f, Hero.Yaw, 0.0f).RotateVector(FVector(X, Y, 0.0f)); return FVector2D(Hero.Pos.X + W.X, Hero.Pos.Y + W.Y); };
				Place.Landmarks.Add({ TEXT("1719 kitchen"), InHouse(220.0f, 130.0f) });
				Place.Landmarks.Add({ TEXT("1719 living room"), InHouse(-150.0f, 380.0f) });
				Place.Landmarks.Add({ TEXT("1719 hall"), InHouse(0.0f, 0.0f) });
				Place.Landmarks.Add({ TEXT("1719 basement"), InHouse(0.0f, 250.0f) });
				UE_LOG(LogTemp, Display, TEXT("RealPlace: 1719 Avenue E at X=%.0f Y=%.0f yaw=%.1f"), Hero.Pos.X, Hero.Pos.Y, Hero.Yaw);
				Place.Lots[Nearest].Width = 900.0f;
				Place.Lots[Nearest].Depth = 1100.0f;
			}
		}
		for (const TSharedPtr<FJsonValue>& V : Root->GetArrayField(TEXT("places")))
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			const TArray<TSharedPtr<FJsonValue>>& P = O->GetArrayField(TEXT("p"));
			if (P.Num() == 2)
			{
				Place.Landmarks.Add({ O->GetStringField(TEXT("name")), ToWorld(P[0]->AsNumber(), P[1]->AsNumber()) });
			}
		}

		// Places that matter to the player but that OSM doesn't name. 1733 Avenue E: Avenue E runs east-west about
		// 770 m north of Bayliss Park; 17th Street crosses it 1.45 km west and 18th Street 125 m further on.
		// 343 East Graham Avenue (built 1920, 1,539 sq ft, per public records): placed on the mapped Graham Avenue East; verify.
		Place.Landmarks.Add({ TEXT("343 East Graham"), ToWorld(1044.0, -215.0) });
		Place.Landmarks.Add({ TEXT("8th and Broadway"), ToWorld(-40.0, 250.0) });
		Place.Landmarks.Add({ TEXT("18th and E"), ToWorld(-1580.0, 770.0) });

		Place.bLoaded = true;
		bReady.store(true);
		UE_LOG(LogTemp, Display, TEXT("RealPlace: Council Bluffs loaded: %d road segments, %d buildings, %d landmarks; origin X=%.0f Y=%.0f"),
			Place.Segments.Num(), Place.Buildings.Num(), Place.Landmarks.Num(), WorldOrigin.X, WorldOrigin.Y);
		return true;
	}

	bool Covers(const FVector2D& Min, const FVector2D& Max, double Margin)
	{
		return bReady.load() && !(Max.X < Place.Min.X - Margin || Min.X > Place.Max.X + Margin || Max.Y < Place.Min.Y - Margin || Min.Y > Place.Max.Y + Margin);
	}

	bool Apply(double X, double Y, FWorldSample& S)
	{
		if (!bReady.load())
		{
			return false;
		}
		// Distance outside the data box (0 inside).
		const double DX = FMath::Max(FMath::Max(Place.Min.X - X, X - Place.Max.X), 0.0);
		const double DY = FMath::Max(FMath::Max(Place.Min.Y - Y, Y - Place.Max.Y), 0.0);
		const double Outside = FMath::Sqrt(DX * DX + DY * DY);
		if (Outside >= BlendMargin)
		{
			return false;
		}
		const float Weight = 1.0f - Smooth(static_cast<float>(Outside / BlendMargin));

		const float Water = WaterFraction(X, Y);
		const float Ground = FMath::Max(ElevationAt(X, Y) - BaseElevation, 0.7f) * 100.0f;
		const float RealHeight = FMath::Lerp(Ground, -350.0f, Water);
		S.Height = FMath::Lerp(S.Height, RealHeight, Weight);
		if (Outside > 0.0)
		{
			return true;
		}

		const int32 RowX = FMath::Clamp(FMath::FloorToInt((X - Place.Min.X) / RasterCell), 0, Place.RasterH - 1);
		const int32 Col = FMath::Clamp(FMath::FloorToInt((Y - Place.Min.Y) / RasterCell), 0, Place.RasterW - 1);
		const uint8 Cell = Place.Raster[RowX * Place.RasterW + Col];
		const uint8 Cover = Cell & CoverMask;

		S.Land = 1.0f;
		S.Mountains = 0.0f;
		S.Volcano = 0.0f;
		S.Snow = 0.0f;
		S.Rock = 0.0f;
		S.Sand = Water > 0.0f ? 1.0f : 0.0f;
		S.Temperature = 0.5f;
		S.Moisture = 0.6f;
		S.Dryness = 0.25f;
		S.Wetness = Water > 0.5f ? 1.0f : 0.0f;
		S.Biome = Water > 0.5f ? EBiome::Ocean : Cover == CoverWood ? EBiome::Forest : EBiome::Grassland;
		float Forest = 0.0f, Trees = 0.0f;
		FLinearColor Color = WorldGen::Srgb(0x6E8B3D);
		switch (Cover)
		{
		case CoverWood: Forest = 1.0f; Trees = 1.0f; Color = WorldGen::Srgb(0x4C6B2C); break;
		case CoverResidential: Trees = 0.16f; Color = WorldGen::Srgb(0x76913F); break;
		case CoverCommercial: Trees = 0.02f; Color = WorldGen::Srgb(0x8A8A70); break;
		case CoverPark: Trees = 0.10f; Color = WorldGen::Srgb(0x6FA03E); break;
		case CoverCemetery: Trees = 0.12f; Color = WorldGen::Srgb(0x6FA03E); break;
		default: Trees = 0.05f; Color = WorldGen::Srgb(0x9A9A4A); S.Dryness = 0.4f; break;
		}
		if (Cell & CoverBuilding)
		{
			Trees = 0.0f;
		}
		if (Water > 0.0f)
		{
			Trees = 0.0f;
			Forest = 0.0f;
		}
		S.Forest = Forest;
		S.TreeDensity = Trees;
		S.Color = Color;
		return true;
	}

	void AppendPaths(const FVector2D& Min, const FVector2D& Max, double Margin, TArray<WorldGen::FPath>& Out)
	{
		if (!bReady.load() || Max.X < Place.Min.X - Margin || Min.X > Place.Max.X + Margin || Max.Y < Place.Min.Y - Margin || Min.Y > Place.Max.Y + Margin)
		{
			return;
		}
		TSet<int32> Seen;
		for (int32 BY = FMath::FloorToInt((Min.Y - Margin) / RoadBucket); BY <= FMath::FloorToInt((Max.Y + Margin) / RoadBucket); ++BY)
		{
			for (int32 BX = FMath::FloorToInt((Min.X - Margin) / RoadBucket); BX <= FMath::FloorToInt((Max.X + Margin) / RoadBucket); ++BX)
			{
				const TArray<int32>* Bucket = Place.RoadGrid.Find(FIntPoint(BX, BY));
				if (!Bucket)
				{
					continue;
				}
				for (const int32 Index : *Bucket)
				{
					bool bAlready = false;
					Seen.Add(Index, &bAlready);
					if (bAlready)
					{
						continue;
					}
					const FSegment& Seg = Place.Segments[Index];
					const FVector2D Lo(FMath::Min(Seg.A.X, Seg.B.X) - Margin, FMath::Min(Seg.A.Y, Seg.B.Y) - Margin);
					const FVector2D Hi(FMath::Max(Seg.A.X, Seg.B.X) + Margin, FMath::Max(Seg.A.Y, Seg.B.Y) + Margin);
					if (Hi.X >= Min.X && Lo.X <= Max.X && Hi.Y >= Min.Y && Lo.Y <= Max.Y)
					{
						WorldGen::FPath P;
						P.A = Seg.A;
						P.B = Seg.B;
						P.Width = Seg.Width;
						P.bRoad = Seg.bRoad;
						P.Seed = Seg.Seed;
						P.bStraight = true;
						Out.Add(P);
					}
				}
			}
		}
	}

	bool NearestRoad(const FVector2D& P, double MaxDist, FVector2D& OutPoint, float& OutWidth)
	{
		if (!bReady.load())
		{
			return false;
		}
		double Best = MaxDist * MaxDist;
		bool bFound = false;
		for (int32 BY = FMath::FloorToInt((P.Y - MaxDist) / RoadBucket); BY <= FMath::FloorToInt((P.Y + MaxDist) / RoadBucket); ++BY)
		{
			for (int32 BX = FMath::FloorToInt((P.X - MaxDist) / RoadBucket); BX <= FMath::FloorToInt((P.X + MaxDist) / RoadBucket); ++BX)
			{
				const TArray<int32>* Bucket = Place.RoadGrid.Find(FIntPoint(BX, BY));
				if (!Bucket)
				{
					continue;
				}
				for (const int32 Index : *Bucket)
				{
					const FSegment& Seg = Place.Segments[Index];
					if (!Seg.bRoad)
					{
						continue;
					}
					const FVector2D AB = Seg.B - Seg.A;
					const double T = FMath::Clamp(FVector2D::DotProduct(P - Seg.A, AB) / FMath::Max(AB.SizeSquared(), 1.0), 0.0, 1.0);
					const FVector2D Q = Seg.A + AB * T;
					const double D2 = FVector2D::DistSquared(P, Q);
					if (D2 < Best)
					{
						Best = D2;
						OutPoint = Q;
						OutWidth = Seg.Width;
						bFound = true;
					}
				}
			}
		}
		return bFound;
	}

	bool Occupied(const FVector2D& P)
	{
		if (!bReady.load())
		{
			return false;
		}
		const int32 BX = FMath::FloorToInt(P.X / RoadBucket), BY = FMath::FloorToInt(P.Y / RoadBucket);
		for (int32 DY = -1; DY <= 1; ++DY)
		{
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				if (const TArray<int32>* Bucket = Place.BuildingGrid.Find(FIntPoint(BX + DX, BY + DY)))
				{
					for (const int32 Index : *Bucket)
					{
						const FBuilding& B = Place.Buildings[Index];
						if (FVector2D::DistSquared(B.Centroid, P) > 9.0e6)
						{
							continue;
						}
						bool bInside = false;
						double MinEdge = 1.0e12;
						for (int32 i = 0, j = B.Outline.Num() - 1; i < B.Outline.Num(); j = i++)
						{
							const FVector2D& A = B.Outline[i];
							const FVector2D& C = B.Outline[j];
							if (((A.X > P.X) != (C.X > P.X)) && (P.Y < (C.Y - A.Y) * (P.X - A.X) / (C.X - A.X) + A.Y))
							{
								bInside = !bInside;
							}
							const FVector2D AB = C - A;
							const double U = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / FMath::Max(AB.SizeSquared(), 1.0), 0.0, 1.0);
							MinEdge = FMath::Min(MinEdge, static_cast<double>(FVector2D::DistSquared(P, A + AB * U)));
					}
						if (bInside || MinEdge < 150.0 * 150.0)
						{
							return true;
						}
					}
				}
				if (const TArray<int32>* Bucket = Place.LotGrid.Find(FIntPoint(BX + DX, BY + DY)))
				{
					for (const int32 Index : *Bucket)
					{
						const FLot& L = Place.Lots[Index];
						const FVector Local = FRotator(0.0f, -L.Yaw, 0.0f).RotateVector(FVector(P.X - L.Pos.X, P.Y - L.Pos.Y, 0.0));
						if (FMath::Abs(Local.X) < L.Width * 0.5f + 150.0f && Local.Y > -L.Depth * 0.5f - 150.0f && Local.Y < L.Depth * 0.5f + 320.0f)
						{
							return true;
						}
					}
				}
			}
		}
		return false;
	}

	void LotsIn(const FVector2D& Min, const FVector2D& Max, TArray<const FLot*>& Out)
	{
		if (!bReady.load())
		{
			return;
		}
		for (int32 BY = FMath::FloorToInt(Min.Y / RoadBucket); BY <= FMath::FloorToInt(Max.Y / RoadBucket); ++BY)
		{
			for (int32 BX = FMath::FloorToInt(Min.X / RoadBucket); BX <= FMath::FloorToInt(Max.X / RoadBucket); ++BX)
			{
				if (const TArray<int32>* Bucket = Place.LotGrid.Find(FIntPoint(BX, BY)))
				{
					for (const int32 Index : *Bucket)
					{
						const FLot& L = Place.Lots[Index];
						if (L.Pos.X >= Min.X && L.Pos.X < Max.X && L.Pos.Y >= Min.Y && L.Pos.Y < Max.Y)
						{
							Out.Add(&L);
						}
					}
				}
			}
		}
	}

	void BuildingsIn(const FVector2D& Min, const FVector2D& Max, TArray<const FBuilding*>& Out)
	{
		if (!bReady.load())
		{
			return;
		}
		for (int32 BY = FMath::FloorToInt(Min.Y / RoadBucket); BY <= FMath::FloorToInt(Max.Y / RoadBucket); ++BY)
		{
			for (int32 BX = FMath::FloorToInt(Min.X / RoadBucket); BX <= FMath::FloorToInt(Max.X / RoadBucket); ++BX)
			{
				if (const TArray<int32>* Bucket = Place.BuildingGrid.Find(FIntPoint(BX, BY)))
				{
					for (const int32 Index : *Bucket)
					{
						const FBuilding& B = Place.Buildings[Index];
						if (B.Centroid.X >= Min.X && B.Centroid.X < Max.X && B.Centroid.Y >= Min.Y && B.Centroid.Y < Max.Y)
						{
							Out.Add(&B);
						}
					}
				}
			}
		}
	}
}
