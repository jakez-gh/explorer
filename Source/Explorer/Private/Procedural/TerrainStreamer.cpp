#include "Procedural/TerrainStreamer.h"
#include "Procedural/WorldGen.h"
#include "ProceduralMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

using WorldGen::Srgb;

namespace
{
	// Hash seeds for independent random streams.
	enum ESeed : uint32
	{
		SeedTreeChance = 1,
		SeedTreeJitterX,
		SeedTreeJitterY,
		SeedTreeKind,
		SeedTreeSize,
		SeedTreeTint,
		SeedVillage = 20,
		SeedCity = 40,
		SeedIsland = 60,
	};

	constexpr double VillageCell = 250000.0;
	constexpr double VillageRadius = 20000.0;
	constexpr double CityCell = 900000.0;
	constexpr double CityRadius = 45000.0;
	constexpr double IslandCell = 700000.0;

	float Smooth(float Edge0, float Edge1, float X)
	{
		const float T = FMath::Clamp((X - Edge0) / (Edge1 - Edge0), 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	FLinearColor Jitter(const FLinearColor& Color, float Random01, float Amount = 0.15f)
	{
		return Color * (1.0f + (Random01 * 2.0f - 1.0f) * Amount);
	}

	/** Deterministic random stream for one settlement or landmark. */
	struct FRandom
	{
		int32 X, Y;
		uint32 Seed;
		int32 Counter = 0;
		float Next() { return WorldGen::HashFloat(X, Y, Seed * 7919u + static_cast<uint32>(Counter++)); }
		float Range(float Min, float Max) { return FMath::Lerp(Min, Max, Next()); }
	};
}

ATerrainStreamer::ATerrainStreamer()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> TerrainMat(TEXT("/Game/Explorer/Materials/M_Terrain.M_Terrain"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> PropMat(TEXT("/Game/Explorer/Materials/M_InstanceColor.M_InstanceColor"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> WaterMat(TEXT("/Game/Explorer/Materials/M_Water.M_Water"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeMesh(TEXT("/Engine/BasicShapes/Cone.Cone"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneMesh(TEXT("/Engine/BasicShapes/Plane.Plane"));

	TerrainMaterial = TerrainMat.Object;
	PropMaterial = PropMat.Object;
	PartMeshes = { CylinderMesh.Object, ConeMesh.Object, SphereMesh.Object, CubeMesh.Object };

	Ocean = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Ocean"));
	Ocean->SetupAttachment(RootComponent);
	Ocean->SetStaticMesh(PlaneMesh.Object);
	Ocean->SetMaterial(0, WaterMat.Object);
	Ocean->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Ocean->SetCastShadow(false);
	Ocean->SetMobility(EComponentMobility::Movable);
}

float ATerrainStreamer::GetHeightAtLocation(FVector2D WorldXY) const
{
	return GetActorLocation().Z + WorldGen::Height(WorldXY.X, WorldXY.Y);
}

FIntPoint ATerrainStreamer::WorldToChunk(const FVector& Location) const
{
	return FIntPoint(FMath::FloorToInt(Location.X / ChunkWorldSize()), FMath::FloorToInt(Location.Y / ChunkWorldSize()));
}

bool ATerrainStreamer::ChunkContains(const FIntPoint& Coord, const FVector& Location) const
{
	return WorldToChunk(Location) == Coord;
}

int32 ATerrainStreamer::StepForDistance(float DistanceInChunks) const
{
	if (DistanceInChunks <= 3.0f) return 1;
	if (DistanceInChunks <= 6.0f) return 2;
	if (DistanceInChunks <= 10.0f) return 4;
	if (DistanceInChunks <= 13.0f) return 8;
	return 16;
}

void ATerrainStreamer::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
	if (!PlayerController)
	{
		return;
	}
	FVector ViewLocation;
	FRotator ViewRotation;
	PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
	const FIntPoint Center = WorldToChunk(ViewLocation);

	// The ocean is one big plane at sea level that follows the viewer, reaching well past the fog so its edge never shows.
	Ocean->SetWorldLocation(FVector(ViewLocation.X, ViewLocation.Y, 0.0));
	Ocean->SetWorldScale3D(FVector(200000.0, 200000.0, 1.0));

	// Recycle chunks outside the view disc (with one chunk of hysteresis).
	const int32 UnloadRadiusSq = FMath::Square(ViewRadius + 1);
	for (auto It = Chunks.CreateIterator(); It; ++It)
	{
		if ((It.Key() - Center).SizeSquared() > UnloadRadiusSq)
		{
			ReleaseChunk(It.Value());
			It.RemoveCurrent();
		}
	}

	// Work out what each chunk in view should look like, and queue the ones that don't yet. Nearest first.
	struct FWork
	{
		int32 DistSq;
		FIntPoint Coord;
		int32 Step;
		bool bCollision;
		EProps Props;
	};
	TArray<FWork> Work;
	for (int32 DY = -ViewRadius; DY <= ViewRadius; ++DY)
	{
		for (int32 DX = -ViewRadius; DX <= ViewRadius; ++DX)
		{
			const int32 DistSq = DX * DX + DY * DY;
			if (DistSq > ViewRadius * ViewRadius)
			{
				continue;
			}
			const FIntPoint Coord = Center + FIntPoint(DX, DY);
			const int32 Step = StepForDistance(FMath::Sqrt(static_cast<float>(DistSq)));
			const bool bCollision = DistSq <= CollisionRadius * CollisionRadius;
			const EProps Props = DistSq <= DetailRadius * DetailRadius ? EProps::Full : EProps::Landmarks;

			const FChunk* Existing = Chunks.Find(Coord);
			if (!Existing || Existing->Step != Step || (bCollision && !Existing->bHasCollision) || Existing->Props != Props)
			{
				Work.Add({ DistSq, Coord, Step, bCollision, Props });
			}
		}
	}
	Work.Sort([](const FWork& A, const FWork& B) { return A.DistSq < B.DistSq; });

	const double Deadline = FPlatformTime::Seconds() + BuildBudgetMs * 0.001;
	for (int32 i = 0; i < Work.Num(); ++i)
	{
		if (i > 0 && FPlatformTime::Seconds() > Deadline)
		{
			break;
		}
		const FWork& Item = Work[i];
		FChunk& Chunk = Chunks.FindOrAdd(Item.Coord);
		if (!Chunk.Mesh || Chunk.Step != Item.Step || (Item.bCollision && !Chunk.bHasCollision))
		{
			BuildTerrain(Item.Coord, Chunk, Item.Step, Item.bCollision);
		}
		if (Chunk.Props != Item.Props)
		{
			BuildProps(Item.Coord, Chunk, Item.Props);
		}
	}
}

void ATerrainStreamer::BuildTerrain(const FIntPoint& Coord, FChunk& Chunk, int32 Step, bool bWithCollision)
{
	const int32 N = ChunkResolution / Step;
	const float Spacing = GridSpacing * Step;
	const int32 Side = N + 1;
	const FVector Origin(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize(), 0.0);

	// Samples with a one-vertex border so normals at chunk edges match the neighbours.
	const int32 PaddedSide = Side + 2;
	TArray<FWorldSample> Samples;
	Samples.SetNum(PaddedSide * PaddedSide);
	for (int32 Y = 0; Y < PaddedSide; ++Y)
	{
		for (int32 X = 0; X < PaddedSide; ++X)
		{
			Samples[Y * PaddedSide + X] = WorldGen::Sample(Origin.X + (X - 1) * Spacing, Origin.Y + (Y - 1) * Spacing);
		}
	}
	auto At = [&](int32 X, int32 Y) -> const FWorldSample& { return Samples[(Y + 1) * PaddedSide + (X + 1)]; };

	TArray<FVector> Vertices;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> Colors;
	TArray<int32> Triangles;
	const int32 NumSkirt = 4 * N;
	Vertices.Reserve(Side * Side + NumSkirt);
	Normals.Reserve(Side * Side + NumSkirt);
	UVs.Reserve(Side * Side + NumSkirt);
	Colors.Reserve(Side * Side + NumSkirt);
	Triangles.Reserve(N * N * 6 + NumSkirt * 12);

	const FLinearColor Rock = WorldGen::RockColor();
	const FLinearColor Snow = WorldGen::SnowColor();
	for (int32 Y = 0; Y < Side; ++Y)
	{
		for (int32 X = 0; X < Side; ++X)
		{
			const FWorldSample& S = At(X, Y);
			Vertices.Add(FVector(X * Spacing, Y * Spacing, S.Height));

			// Central-difference normal from neighbouring heights.
			const float DX = At(X + 1, Y).Height - At(X - 1, Y).Height;
			const float DY = At(X, Y + 1).Height - At(X, Y - 1).Height;
			const FVector Normal = FVector(-DX, -DY, 2.0f * Spacing).GetSafeNormal();
			Normals.Add(Normal);

			// Bare rock on steep ground above the shoreline; snow still clings to gentler slopes in the cold.
			FLinearColor Color = S.Color;
			const float Steep = Smooth(0.82f, 0.62f, Normal.Z) * Smooth(200.0f, 800.0f, S.Height);
			Color = FMath::Lerp(Color, Rock, Steep);
			const float SnowCover = Smooth(0.16f, 0.06f, S.Temperature) * Smooth(0.5f, 0.7f, Normal.Z);
			Color = FMath::Lerp(Color, Snow, SnowCover);
			FColor Packed = Color.ToFColor(false);
			Packed.A = static_cast<uint8>(FMath::Clamp(S.Wetness, 0.0f, 1.0f) * 255.0f);
			Colors.Add(Packed);

			UVs.Add(FVector2D(X, Y));
		}
	}

	// Same winding as UKismetProceduralMeshLibrary::CreateGridMeshWelded, so faces point up.
	for (int32 Y = 0; Y < N; ++Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			const int32 BottomLeft = Y * Side + X;
			const int32 BottomRight = BottomLeft + 1;
			const int32 TopLeft = BottomLeft + Side;
			const int32 TopRight = TopLeft + 1;
			Triangles.Append({ BottomLeft, TopLeft, BottomRight, BottomRight, TopLeft, TopRight });
		}
	}

	// Skirts: a curtain hanging from the edge hides cracks where neighbours use a different level of detail.
	TArray<int32> Perimeter;
	Perimeter.Reserve(NumSkirt + 1);
	for (int32 X = 0; X < N; ++X) Perimeter.Add(X);
	for (int32 Y = 0; Y < N; ++Y) Perimeter.Add(Y * Side + N);
	for (int32 X = N; X > 0; --X) Perimeter.Add(N * Side + X);
	for (int32 Y = N; Y > 0; --Y) Perimeter.Add(Y * Side);
	Perimeter.Add(0);

	const float SkirtDepth = 3000.0f * Step;
	const int32 SkirtStart = Vertices.Num();
	for (int32 i = 0; i < NumSkirt; ++i)
	{
		// Copy before adding: Add() may reallocate the array the reference points into.
		const int32 Top = Perimeter[i];
		const FVector Vertex = Vertices[Top] - FVector(0, 0, SkirtDepth);
		const FVector Normal = Normals[Top];
		const FColor Color = Colors[Top];
		const FVector2D UV = UVs[Top];
		Vertices.Add(Vertex);
		Normals.Add(Normal);
		Colors.Add(Color);
		UVs.Add(UV);
	}
	for (int32 i = 0; i < NumSkirt; ++i)
	{
		const int32 A = Perimeter[i];
		const int32 B = Perimeter[i + 1];
		const int32 A2 = SkirtStart + i;
		const int32 B2 = SkirtStart + (i + 1) % NumSkirt;
		// Both windings, so the curtain is visible from either side.
		Triangles.Append({ A, A2, B, B, A2, B2, A, B, A2, B, B2, A2 });
	}

	if (!Chunk.Mesh)
	{
		Chunk.Mesh = AcquireMesh();
	}
	Chunk.Mesh->SetWorldLocation(Origin);
	Chunk.Mesh->CreateMeshSection(0, Vertices, Triangles, Normals, UVs, Colors, TArray<FProcMeshTangent>(), bWithCollision);
	Chunk.Mesh->SetMaterial(0, TerrainMaterial);
	Chunk.Step = Step;
	Chunk.bHasCollision = bWithCollision;
}

void ATerrainStreamer::FPropBatch::Add(EPropPart Part, const FVector& Center, const FRotator& Rotation, const FVector& SizeCm, const FLinearColor& Color, float Glow)
{
	// Engine basic shapes are 100 units across and centred on their pivot.
	Transforms[Part].Add(FTransform(Rotation, Center, SizeCm / 100.0));
	CustomData[Part].Append({ Color.R, Color.G, Color.B, Glow });
}

void ATerrainStreamer::BuildProps(const FIntPoint& Coord, FChunk& Chunk, EProps Level)
{
	FPropBatch Batch;
	AddCities(Coord, Batch);
	AddFloatingIslands(Coord, Batch);
	if (Level == EProps::Full)
	{
		AddVegetation(Coord, Batch);
		AddVillages(Coord, Batch);
	}

	for (int32 Part = 0; Part < NumParts; ++Part)
	{
		UHierarchicalInstancedStaticMeshComponent*& Component = Chunk.Parts[Part];
		const TArray<FTransform>& Transforms = Batch.Transforms[Part];
		if (Transforms.Num() == 0)
		{
			if (Component)
			{
				Component->ClearInstances();
				Component->SetVisibility(false);
				PartPools[Part].Add(Component);
				Component = nullptr;
			}
			continue;
		}

		if (!Component)
		{
			Component = AcquirePart(static_cast<EPropPart>(Part));
		}
		Component->ClearInstances();
		Component->AddInstances(Transforms, false, true);
		const TArray<float>& Data = Batch.CustomData[Part];
		for (int32 i = 0; i < Transforms.Num(); ++i)
		{
			Component->SetCustomData(i, TArrayView<const float>(&Data[i * 4], 4), false);
		}
		Component->MarkRenderStateDirty();
	}
	Chunk.Props = Level;
}

void ATerrainStreamer::AddVegetation(const FIntPoint& Coord, FPropBatch& Batch) const
{
	constexpr double Cell = 1600.0;
	const int32 CellsPerChunk = FMath::RoundToInt(ChunkWorldSize() / Cell);
	const int32 FirstX = Coord.X * CellsPerChunk;
	const int32 FirstY = Coord.Y * CellsPerChunk;

	// Settlements clear the land around them.
	TArray<TPair<FVector2D, double>> Clearings;
	const FVector2D ChunkCenter((Coord.X + 0.5) * ChunkWorldSize(), (Coord.Y + 0.5) * ChunkWorldSize());
	auto GatherClearings = [&](double CellSize, double Radius, TFunctionRef<bool(int32, int32, FVector&)> Find)
	{
		const double Reach = ChunkWorldSize() + Radius;
		for (int32 CY = FMath::FloorToInt((ChunkCenter.Y - Reach) / CellSize); CY <= FMath::FloorToInt((ChunkCenter.Y + Reach) / CellSize); ++CY)
		{
			for (int32 CX = FMath::FloorToInt((ChunkCenter.X - Reach) / CellSize); CX <= FMath::FloorToInt((ChunkCenter.X + Reach) / CellSize); ++CX)
			{
				FVector Center;
				if (Find(CX, CY, Center) && FVector2D::Distance(FVector2D(Center), ChunkCenter) < Reach)
				{
					Clearings.Emplace(FVector2D(Center), Radius);
				}
			}
		}
	};
	GatherClearings(VillageCell, VillageRadius + 3000.0, [](int32 X, int32 Y, FVector& P) { return FindVillage(X, Y, P); });
	GatherClearings(CityCell, CityRadius + 3000.0, [](int32 X, int32 Y, FVector& P) { return FindCity(X, Y, P); });

	const FLinearColor Trunk = Srgb(0x5A4030);
	for (int32 CY = FirstY; CY < FirstY + CellsPerChunk; ++CY)
	{
		for (int32 CX = FirstX; CX < FirstX + CellsPerChunk; ++CX)
		{
			const float Chance = WorldGen::HashFloat(CX, CY, SeedTreeChance);
			const double PX = (CX + WorldGen::HashFloat(CX, CY, SeedTreeJitterX)) * Cell;
			const double PY = (CY + WorldGen::HashFloat(CX, CY, SeedTreeJitterY)) * Cell;
			bool bCleared = false;
			for (const TPair<FVector2D, double>& Clearing : Clearings)
			{
				bCleared |= FVector2D::DistSquared(Clearing.Key, FVector2D(PX, PY)) < FMath::Square(Clearing.Value);
			}
			if (bCleared)
			{
				continue;
			}
			const FWorldSample S = WorldGen::Sample(PX, PY);
			if (S.Height < 150.0f)
			{
				continue;
			}

			const float BoulderChance = 0.012f + S.Mountains * 0.05f + (S.Biome == EBiome::Desert ? 0.01f : 0.0f);
			const float Kind = WorldGen::HashFloat(CX, CY, SeedTreeKind);
			const float Size = FMath::Lerp(0.7f, 1.4f, WorldGen::HashFloat(CX, CY, SeedTreeSize));
			const float Tint = WorldGen::HashFloat(CX, CY, SeedTreeTint);
			const FVector Ground(PX, PY, S.Height);

			if (Chance < S.TreeDensity)
			{
				// Trees avoid cliffs.
				const float Slope = FMath::Max(
					FMath::Abs(WorldGen::Height(PX + 300.0, PY) - S.Height),
					FMath::Abs(WorldGen::Height(PX, PY + 300.0) - S.Height)) / 300.0f;
				if (Slope > 0.8f)
				{
					continue;
				}

				switch (S.Biome)
				{
				case EBiome::Desert:
				{
					// Saguaro: a column with one or two arms.
					const float H = 400.0f * Size;
					Batch.Add(Cylinder, Ground + FVector(0, 0, H * 0.5f), FRotator::ZeroRotator, FVector(90, 90, H), Jitter(Srgb(0x4F7A3A), Tint));
					const int32 Arms = Kind < 0.5f ? 1 : 2;
					for (int32 Arm = 0; Arm < Arms; ++Arm)
					{
						const float Side = Arm == 0 ? 1.0f : -1.0f;
						const float ArmH = H * FMath::Lerp(0.3f, 0.45f, Tint);
						Batch.Add(Cylinder, Ground + FVector(Side * 110.0f, 0, H * 0.45f + ArmH * 0.5f), FRotator::ZeroRotator, FVector(70, 70, ArmH), Jitter(Srgb(0x4F7A3A), Kind));
					}
					break;
				}
				case EBiome::Savanna:
				{
					// Acacia: short trunk, flat wide crown.
					const float H = 550.0f * Size;
					Batch.Add(Cylinder, Ground + FVector(0, 0, H * 0.5f), FRotator::ZeroRotator, FVector(60, 60, H), Trunk);
					Batch.Add(Sphere, Ground + FVector(0, 0, H + 80.0f), FRotator::ZeroRotator, FVector(1000, 1000, 220) * Size, Jitter(Srgb(0x6E7A34), Tint));
					break;
				}
				case EBiome::Jungle:
				{
					// Tall canopy trees with layered crowns.
					const float H = 1800.0f * Size;
					Batch.Add(Cylinder, Ground + FVector(0, 0, H * 0.5f), FRotator::ZeroRotator, FVector(110, 110, H), Trunk);
					Batch.Add(Sphere, Ground + FVector(0, 0, H), FRotator::ZeroRotator, FVector(1300, 1300, 500) * Size, Jitter(Srgb(0x245C1E), Tint));
					Batch.Add(Sphere, Ground + FVector(0, 0, H * 0.65f), FRotator::ZeroRotator, FVector(800, 800, 350) * Size, Jitter(Srgb(0x2E6B26), Kind));
					break;
				}
				case EBiome::Taiga:
				case EBiome::Tundra:
				case EBiome::Snow:
				{
					// Conifer; frosted in the deep cold.
					const float H = 1200.0f * Size;
					Batch.Add(Cylinder, Ground + FVector(0, 0, 150.0f), FRotator::ZeroRotator, FVector(70, 70, 300), Trunk);
					const FLinearColor Needles = FMath::Lerp(Srgb(0x2C4A2E), Srgb(0xDDE6E6), Smooth(0.22f, 0.08f, S.Temperature));
					Batch.Add(Cone, Ground + FVector(0, 0, 200.0f + H * 0.5f), FRotator::ZeroRotator, FVector(H * 0.4f, H * 0.4f, H), Jitter(Needles, Tint));
					break;
				}
				default:
				{
					// Broadleaf, with the odd conifer and a few trees already turning gold and red.
					if (Kind < 0.2f)
					{
						const float H = 1100.0f * Size;
						Batch.Add(Cylinder, Ground + FVector(0, 0, 150.0f), FRotator::ZeroRotator, FVector(70, 70, 300), Trunk);
						Batch.Add(Cone, Ground + FVector(0, 0, 200.0f + H * 0.5f), FRotator::ZeroRotator, FVector(H * 0.4f, H * 0.4f, H), Jitter(Srgb(0x2F5233), Tint));
						break;
					}
					const float H = 550.0f * Size;
					const float Crown = 700.0f * Size;
					FLinearColor Leaves = Srgb(0x3E6B2A);
					if (Kind > 0.9f) Leaves = Srgb(0xC9A23A);
					else if (Kind > 0.82f) Leaves = Srgb(0xB5652A);
					Batch.Add(Cylinder, Ground + FVector(0, 0, H * 0.5f), FRotator::ZeroRotator, FVector(80, 80, H), Trunk);
					Batch.Add(Sphere, Ground + FVector(0, 0, H + Crown * 0.3f), FRotator::ZeroRotator, FVector(Crown, Crown, Crown * 1.15f), Jitter(Leaves, Tint));
					break;
				}
				}
			}
			else if (Chance < S.TreeDensity + BoulderChance)
			{
				const float B = FMath::Lerp(200.0f, 700.0f, Kind) * Size;
				const FRotator Spin(0, Tint * 360.0f, 0);
				Batch.Add(Sphere, Ground + FVector(0, 0, B * 0.15f), Spin, FVector(B, B * 0.8f, B * 0.6f), Jitter(Srgb(0x8A847C), Tint, 0.2f));
			}
		}
	}
}

double ATerrainStreamer::VillageCellSize() { return VillageCell; }
double ATerrainStreamer::CityCellSize() { return CityCell; }
double ATerrainStreamer::IslandCellSize() { return IslandCell; }

bool ATerrainStreamer::FindVillage(int32 CX, int32 CY, FVector& OutCenter)
{
	FRandom Rand{ CX, CY, SeedVillage };
	if (Rand.Next() > 0.3f)
	{
		return false;
	}
	OutCenter = FVector((CX + Rand.Range(0.2f, 0.8f)) * VillageCell, (CY + Rand.Range(0.2f, 0.8f)) * VillageCell, 0.0);
	const FWorldSample S = WorldGen::Sample(OutCenter.X, OutCenter.Y);
	OutCenter.Z = S.Height;
	return S.Height >= 300.0f && S.Height <= 30000.0f && S.Mountains <= 0.3f && S.Biome != EBiome::Beach && S.Biome != EBiome::Snow;
}

bool ATerrainStreamer::FindCity(int32 CX, int32 CY, FVector& OutCenter)
{
	FRandom Rand{ CX, CY, SeedCity };
	if (Rand.Next() > 0.22f)
	{
		return false;
	}
	OutCenter = FVector((CX + Rand.Range(0.25f, 0.75f)) * CityCell, (CY + Rand.Range(0.25f, 0.75f)) * CityCell, 0.0);
	const FWorldSample S = WorldGen::Sample(OutCenter.X, OutCenter.Y);
	OutCenter.Z = S.Height;
	return S.Height >= 300.0f && S.Height <= 15000.0f && S.Mountains <= 0.15f && S.Biome != EBiome::Snow && S.Biome != EBiome::Jungle;
}

bool ATerrainStreamer::FindFloatingIsland(int32 CX, int32 CY, FVector& OutTop, float& OutRadius)
{
	FRandom Rand{ CX, CY, SeedIsland };
	if (Rand.Next() > 0.35f)
	{
		return false;
	}
	OutTop = FVector((CX + Rand.Next()) * IslandCell, (CY + Rand.Next()) * IslandCell, 0.0);
	OutTop.Z = FMath::Max(WorldGen::Height(OutTop.X, OutTop.Y), 0.0f) + Rand.Range(40000.0f, 100000.0f);
	OutRadius = Rand.Range(6000.0f, 24000.0f);
	return true;
}

void ATerrainStreamer::AddVillages(const FIntPoint& Coord, FPropBatch& Batch) const
{
	const FVector ChunkMin(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize(), 0.0);
	const int32 MinCX = FMath::FloorToInt((ChunkMin.X - VillageRadius) / VillageCell);
	const int32 MaxCX = FMath::FloorToInt((ChunkMin.X + ChunkWorldSize() + VillageRadius) / VillageCell);
	const int32 MinCY = FMath::FloorToInt((ChunkMin.Y - VillageRadius) / VillageCell);
	const int32 MaxCY = FMath::FloorToInt((ChunkMin.Y + ChunkWorldSize() + VillageRadius) / VillageCell);

	for (int32 CY = MinCY; CY <= MaxCY; ++CY)
	{
		for (int32 CX = MinCX; CX <= MaxCX; ++CX)
		{
			FVector Center;
			if (!FindVillage(CX, CY, Center))
			{
				continue;
			}
			const FWorldSample S = WorldGen::Sample(Center.X, Center.Y);
			FRandom Rand{ CX, CY, SeedVillage + 2 };

			// Building style follows the climate.
			const bool bAdobe = S.Biome == EBiome::Desert || S.Biome == EBiome::Savanna;
			const bool bTimber = S.Biome == EBiome::Taiga || S.Biome == EBiome::Tundra;
			const FLinearColor Walls[] = { Srgb(0xEDE3CF), Srgb(0xF4F1EA), Srgb(0xD9C3A0), Srgb(0xE8C9A8) };
			const FLinearColor Roofs[] = { Srgb(0x9A3F2C), Srgb(0x5B4A44), Srgb(0x7A4B2A), Srgb(0x44505A) };
			const FLinearColor Lamp = Srgb(0xFFC37A);

			const int32 Houses = 6 + FMath::FloorToInt(Rand.Next() * 14.0f);
			for (int32 i = 0; i < Houses; ++i)
			{
				FRandom House{ CX * 131 + i, CY, SeedVillage + 1 };
				const float Angle = House.Range(0.0f, 2.0f * PI);
				const float Radius = i == 0 ? 0.0f : 2500.0f + 17000.0f * FMath::Sqrt(House.Next());
				const FVector Pos = Center + FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, 0.0);
				if (!ChunkContains(Coord, Pos))
				{
					continue;
				}
				const float Ground = WorldGen::Height(Pos.X, Pos.Y);
				if (Ground < 150.0f)
				{
					continue;
				}
				const float Yaw = House.Range(0.0f, 360.0f);

				if (i == 0)
				{
					// A tower or temple at the heart of the village.
					const FLinearColor Stone = bAdobe ? Srgb(0xD8B98A) : Srgb(0xCFC8BA);
					Batch.Add(Cube, FVector(Pos.X, Pos.Y, Ground + 650.0f), FRotator(0, Yaw, 0), FVector(550, 550, 1500), Stone);
					if (bAdobe)
					{
						Batch.Add(Sphere, FVector(Pos.X, Pos.Y, Ground + 1400.0f), FRotator::ZeroRotator, FVector(520, 520, 520), Srgb(0xF0E6D2));
					}
					else
					{
						Batch.Add(Cone, FVector(Pos.X, Pos.Y, Ground + 1400.0f + 450.0f), FRotator(0, Yaw, 0), FVector(600, 600, 900), Roofs[1]);
					}
					Batch.Add(Sphere, FVector(Pos.X, Pos.Y, Ground + 1250.0f), FRotator::ZeroRotator, FVector(90, 90, 90), Lamp, 12.0f);
					continue;
				}

				const float W = House.Range(600.0f, 1000.0f);
				const float D = House.Range(500.0f, 800.0f);
				const float H = House.Range(400.0f, 600.0f);
				const FLinearColor Wall = bTimber ? Srgb(0x6B4A33) : bAdobe ? Srgb(0xD4B083) : Walls[FMath::FloorToInt(House.Next() * 4) % 4];
				Batch.Add(Cube, FVector(Pos.X, Pos.Y, Ground + H * 0.5f - 50.0f), FRotator(0, Yaw, 0), FVector(W, D, H), Jitter(Wall, House.Next(), 0.06f));

				if (!bAdobe)
				{
					// Pitched roof: a cube turned 45 degrees about the ridge, its diagonal spanning the house.
					const float S2 = D / UE_SQRT_2 + 40.0f;
					const FLinearColor Roof = bTimber ? Srgb(0x3F3A36) : Roofs[FMath::FloorToInt(House.Next() * 4) % 4];
					Batch.Add(Cube, FVector(Pos.X, Pos.Y, Ground + H - 50.0f), FRotator(0, Yaw, 45), FVector(W + 60.0f, S2, S2), Roof);
				}

				// A warm lantern by the door.
				const FVector Door = FRotator(0, Yaw, 0).RotateVector(FVector(0, D * 0.5f + 60.0f, 0));
				Batch.Add(Sphere, FVector(Pos.X, Pos.Y, Ground + 230.0f) + Door, FRotator::ZeroRotator, FVector(45, 45, 45), Lamp, 10.0f);
			}
		}
	}
}

void ATerrainStreamer::AddCities(const FIntPoint& Coord, FPropBatch& Batch) const
{
	const FVector ChunkMin(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize(), 0.0);
	const int32 MinCX = FMath::FloorToInt((ChunkMin.X - CityRadius) / CityCell);
	const int32 MaxCX = FMath::FloorToInt((ChunkMin.X + ChunkWorldSize() + CityRadius) / CityCell);
	const int32 MinCY = FMath::FloorToInt((ChunkMin.Y - CityRadius) / CityCell);
	const int32 MaxCY = FMath::FloorToInt((ChunkMin.Y + ChunkWorldSize() + CityRadius) / CityCell);

	for (int32 CY = MinCY; CY <= MaxCY; ++CY)
	{
		for (int32 CX = MinCX; CX <= MaxCX; ++CX)
		{
			FVector Center;
			if (!FindCity(CX, CY, Center))
			{
				continue;
			}

			const FLinearColor Facades[] = { Srgb(0xB8B4AC), Srgb(0x6F8796), Srgb(0xC9B38F), Srgb(0xE6E6E0), Srgb(0x8C96A0) };
			constexpr int32 Blocks = 7;
			constexpr float Spacing = 6000.0f;
			for (int32 GY = -Blocks; GY <= Blocks; ++GY)
			{
				for (int32 GX = -Blocks; GX <= Blocks; ++GX)
				{
					FRandom Tower{ CX * 64 + GX, CY * 64 + GY, SeedCity + 1 };
					const float Falloff = 1.0f - FVector2D(GX, GY).Size() * Spacing / CityRadius;
					if (Falloff <= 0.0f || Tower.Next() > 0.75f)
					{
						continue;
					}
					const FVector Pos = Center + FVector(GX * Spacing + Tower.Range(-800.0f, 800.0f), GY * Spacing + Tower.Range(-800.0f, 800.0f), 0.0);
					if (!ChunkContains(Coord, Pos))
					{
						continue;
					}
					const float Ground = WorldGen::Height(Pos.X, Pos.Y);
					if (Ground < 150.0f)
					{
						continue;
					}
					const float H = FMath::Lerp(2500.0f, 20000.0f, Falloff * Falloff * Tower.Next());
					const float W = Tower.Range(1800.0f, 3400.0f);
					const int32 Style = FMath::FloorToInt(Tower.Next() * 5) % 5;
					const float Glint = Style == 1 || Style == 4 ? 0.06f : 0.0f;
					Batch.Add(Cube, FVector(Pos.X, Pos.Y, Ground + H * 0.5f - 200.0f), FRotator(0, Tower.Range(-4.0f, 4.0f), 0), FVector(W, W * Tower.Range(0.7f, 1.0f), H), Facades[Style], Glint);
					if (H > 12000.0f)
					{
						Batch.Add(Sphere, FVector(Pos.X, Pos.Y, Ground + H), FRotator::ZeroRotator, FVector(150, 150, 150), Srgb(0xFF4A3A), 30.0f);
					}
				}
			}
		}
	}
}

void ATerrainStreamer::AddFloatingIslands(const FIntPoint& Coord, FPropBatch& Batch) const
{
	const FVector ChunkMin(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize(), 0.0);
	const int32 MinCX = FMath::FloorToInt(ChunkMin.X / IslandCell);
	const int32 MaxCX = FMath::FloorToInt((ChunkMin.X + ChunkWorldSize()) / IslandCell);
	const int32 MinCY = FMath::FloorToInt(ChunkMin.Y / IslandCell);
	const int32 MaxCY = FMath::FloorToInt((ChunkMin.Y + ChunkWorldSize()) / IslandCell);

	for (int32 CY = MinCY; CY <= MaxCY; ++CY)
	{
		for (int32 CX = MinCX; CX <= MaxCX; ++CX)
		{
			FVector Top;
			float R;
			if (!FindFloatingIsland(CX, CY, Top, R) || !ChunkContains(Coord, Top))
			{
				continue;
			}
			FRandom Rand{ CX, CY, SeedIsland + 1 };

			// Rocky underside tapering to a point, a soft green cap, and a few trees.
			const float Depth = R * Rand.Range(1.2f, 2.0f);
			Batch.Add(Cone, Top - FVector(0, 0, Depth * 0.5f), FRotator(180, Rand.Range(0.0f, 360.0f), 0), FVector(R * 2.0f, R * 2.0f, Depth), Srgb(0x7A6A5A));
			Batch.Add(Sphere, Top, FRotator::ZeroRotator, FVector(R * 2.1f, R * 2.1f, R * 0.35f), Srgb(0x6E9A48));

			const int32 Trees = 4 + FMath::FloorToInt(Rand.Next() * 10);
			for (int32 i = 0; i < Trees; ++i)
			{
				const float A = Rand.Range(0.0f, 2.0f * PI);
				const float D = R * 0.65f * FMath::Sqrt(Rand.Next());
				const FVector Base = Top + FVector(FMath::Cos(A) * D, FMath::Sin(A) * D, R * 0.12f);
				const float Size = Rand.Range(0.8f, 1.6f);
				Batch.Add(Cylinder, Base + FVector(0, 0, 275.0f * Size), FRotator::ZeroRotator, FVector(80, 80, 550) * Size, Srgb(0x5A4030));
				Batch.Add(Sphere, Base + FVector(0, 0, 760.0f * Size), FRotator::ZeroRotator, FVector(750, 750, 850) * Size, Jitter(Srgb(0x5E9A3A), Rand.Next()));
			}

			// Glowing motes drifting around it.
			const int32 Motes = 3 + FMath::FloorToInt(Rand.Next() * 4);
			for (int32 i = 0; i < Motes; ++i)
			{
				const float A = Rand.Range(0.0f, 2.0f * PI);
				const float D = R * Rand.Range(1.2f, 1.8f);
				const FVector Mote = Top + FVector(FMath::Cos(A) * D, FMath::Sin(A) * D, Rand.Range(-R, R * 0.6f));
				const float Size = Rand.Range(150.0f, 400.0f);
				Batch.Add(Sphere, Mote, FRotator::ZeroRotator, FVector(Size), Srgb(0xFFE7A8), 8.0f);
			}
		}
	}
}

UProceduralMeshComponent* ATerrainStreamer::AcquireMesh()
{
	UProceduralMeshComponent* Mesh = MeshPool.Num() > 0 ? MeshPool.Pop(EAllowShrinking::No).Get() : nullptr;
	if (!Mesh)
	{
		Mesh = NewObject<UProceduralMeshComponent>(this);
		Mesh->bUseAsyncCooking = true;
		Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetupAttachment(RootComponent);
		Mesh->RegisterComponent();
	}
	Mesh->SetVisibility(true);
	return Mesh;
}

UHierarchicalInstancedStaticMeshComponent* ATerrainStreamer::AcquirePart(EPropPart Part)
{
	UHierarchicalInstancedStaticMeshComponent* Component = PartPools[Part].Num() > 0 ? PartPools[Part].Pop(EAllowShrinking::No) : nullptr;
	if (!Component)
	{
		Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(PartMeshes[Part]);
		Component->SetMaterial(0, PropMaterial);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->NumCustomDataFloats = 4;
		Component->SetupAttachment(RootComponent);
		Component->RegisterComponent();
		AllParts.Add(Component);
	}
	Component->SetVisibility(true);
	return Component;
}

void ATerrainStreamer::ReleaseChunk(FChunk& Chunk)
{
	if (Chunk.Mesh)
	{
		Chunk.Mesh->ClearAllMeshSections();
		Chunk.Mesh->SetVisibility(false);
		MeshPool.Add(Chunk.Mesh);
		Chunk.Mesh = nullptr;
	}
	for (int32 Part = 0; Part < NumParts; ++Part)
	{
		if (UHierarchicalInstancedStaticMeshComponent* Component = Chunk.Parts[Part])
		{
			Component->ClearInstances();
			Component->SetVisibility(false);
			PartPools[Part].Add(Component);
			Chunk.Parts[Part] = nullptr;
		}
	}
}
