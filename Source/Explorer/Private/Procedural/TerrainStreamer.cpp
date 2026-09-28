#include "Procedural/TerrainStreamer.h"
#include "Procedural/WorldGen.h"
#include "ProceduralMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
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
		SeedLighthouse = 80,
		SeedStones = 100,
	};

	constexpr double VillageCell = 250000.0;
	constexpr double VillageRadius = 20000.0;
	constexpr double CityCell = 900000.0;
	constexpr double CityRadius = 45000.0;
	constexpr double IslandCell = 900000.0;
	constexpr double LighthouseCell = 500000.0;
	constexpr double StoneCircleCell = 400000.0;
	constexpr double StoneCircleRadius = 2500.0;

	// Vegetation and rocks fade out beyond this distance; terrain colour carries forests further out.
	constexpr float DetailCullDistance = 150000.0f;

	// Far terrain tiles: 4x4 chunks each, dropped slightly so nearer, finer terrain always wins where they overlap.
	constexpr int32 FarTileChunks = 4;
	constexpr int32 FarTileResolution = 32;
	constexpr float FarTileDrop = 2500.0f;

	// Grass tiles align with the 5 m terrain grid so blades sit exactly on the rendered surface.
	constexpr double GrassTileSize = 2000.0;
	constexpr int32 GrassClumpsPerSide = 16;

	float Smooth(float Edge0, float Edge1, float X)
	{
		const float T = FMath::Clamp((X - Edge0) / (Edge1 - Edge0), 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	FLinearColor Jitter(const FLinearColor& Color, float Random01, float Amount = 0.12f)
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

	// Tints multiply the scanned textures, so 1 keeps them as photographed.
	const FLinearColor White(1.0f, 1.0f, 1.0f);
	const FLinearColor Bark(0.33f, 0.24f, 0.17f);
	const FLinearColor LeafTemperate(0.95f, 1.0f, 0.85f);
	const FLinearColor LeafConifer(0.5f, 0.66f, 0.55f);
	const FLinearColor LeafJungle(0.8f, 1.05f, 0.7f);
	const FLinearColor LeafDry(1.25f, 1.05f, 0.6f);
	const FLinearColor LeafGold(1.7f, 1.25f, 0.35f);
	const FLinearColor LeafRust(1.8f, 0.75f, 0.3f);
	const FLinearColor LeafFrost(1.5f, 1.6f, 1.6f);
}

ATerrainStreamer::ATerrainStreamer()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> TerrainMat(TEXT("/Game/Explorer/Materials/M_Terrain.M_Terrain"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> PropMat(TEXT("/Game/Explorer/Materials/M_Prop.M_Prop"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> RockMat(TEXT("/Game/Explorer/Materials/M_RockMesh.M_RockMesh"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> FoliageMat(TEXT("/Game/Explorer/Materials/M_Foliage.M_Foliage"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> WaterMat(TEXT("/Game/Explorer/Materials/M_Water.M_Water"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> GrassMat(TEXT("/Game/Explorer/Materials/M_Grass.M_Grass"));
	GrassMaterial = GrassMat.Object;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeMesh(TEXT("/Engine/BasicShapes/Cone.Cone"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> RockMesh(TEXT("/Game/StarterContent/Props/SM_Rock.SM_Rock"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> BushMesh(TEXT("/Game/StarterContent/Props/SM_Bush.SM_Bush"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneMesh(TEXT("/Engine/BasicShapes/Plane.Plane"));

	TerrainMaterial = TerrainMat.Object;
	PartMeshes = { CylinderMesh.Object, ConeMesh.Object, SphereMesh.Object, CubeMesh.Object, RockMesh.Object, BushMesh.Object, CylinderMesh.Object };
	PartMaterials = { PropMat.Object, PropMat.Object, PropMat.Object, PropMat.Object, RockMat.Object, FoliageMat.Object, PropMat.Object };

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
	if (DistanceInChunks <= 5.0f) return 1;
	if (DistanceInChunks <= 9.0f) return 2;
	if (DistanceInChunks <= 13.0f) return 4;
	return 8;
}

void ATerrainStreamer::BeginPlay()
{
	Super::BeginPlay();
	GrassMesh = CreateGrassClumpMesh();
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

	UpdateGrass(ViewLocation, Deadline);
	UpdateFarTerrain(Center, Deadline);
}

void ATerrainStreamer::UpdateFarTerrain(const FIntPoint& Center, double Deadline)
{
	auto FloorDiv = [](int32 A, int32 B) { return A >= 0 ? A / B : (A - B + 1) / B; };
	const FIntPoint CenterTile(FloorDiv(Center.X, FarTileChunks), FloorDiv(Center.Y, FarTileChunks));
	const int32 TileRadius = FarRadius / FarTileChunks;

	// A tile is redundant once every chunk in it is inside the near disc.
	auto Covered = [&](const FIntPoint& Tile)
	{
		const int32 Limit = FMath::Square(ViewRadius - 1);
		for (const FIntPoint Corner : { FIntPoint(0, 0), FIntPoint(FarTileChunks - 1, 0), FIntPoint(0, FarTileChunks - 1), FIntPoint(FarTileChunks - 1, FarTileChunks - 1) })
		{
			if ((Tile * FarTileChunks + Corner - Center).SizeSquared() > Limit)
			{
				return false;
			}
		}
		return true;
	};

	for (auto It = FarTiles.CreateIterator(); It; ++It)
	{
		if ((It.Key() - CenterTile).SizeSquared() > FMath::Square(TileRadius + 1) || Covered(It.Key()))
		{
			It.Value()->ClearAllMeshSections();
			It.Value()->SetVisibility(false);
			MeshPool.Add(It.Value());
			It.RemoveCurrent();
		}
	}

	TArray<TPair<int32, FIntPoint>> Missing;
	for (int32 DY = -TileRadius; DY <= TileRadius; ++DY)
	{
		for (int32 DX = -TileRadius; DX <= TileRadius; ++DX)
		{
			const FIntPoint Tile = CenterTile + FIntPoint(DX, DY);
			if (DX * DX + DY * DY <= TileRadius * TileRadius && !FarTiles.Contains(Tile) && !Covered(Tile))
			{
				Missing.Emplace(DX * DX + DY * DY, Tile);
			}
		}
	}
	Missing.Sort([](const TPair<int32, FIntPoint>& A, const TPair<int32, FIntPoint>& B) { return A.Key < B.Key; });

	const double TileSize = ChunkWorldSize() * FarTileChunks;
	for (const TPair<int32, FIntPoint>& Item : Missing)
	{
		if (FPlatformTime::Seconds() > Deadline)
		{
			break;
		}
		UProceduralMeshComponent* Mesh = AcquireMesh();
		BuildSurface(Mesh, FVector(Item.Value.X * TileSize, Item.Value.Y * TileSize, -FarTileDrop), FarTileResolution, TileSize / FarTileResolution, false);
		FarTiles.Add(Item.Value, Mesh);
	}
}

UStaticMesh* ATerrainStreamer::CreateGrassClumpMesh()
{
	// A clump of tapered, gently curved blades. Vertex colour R runs 0 at the root to 1 at the tip.
	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	UVs.SetNumChannels(1);
	const FPolygonGroupID Group = Description.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Grass");

	constexpr int32 Blades = 14;
	constexpr int32 Segments = 3;
	for (int32 Blade = 0; Blade < Blades; ++Blade)
	{
		auto Rand = [Blade](uint32 Seed) { return WorldGen::HashFloat(Blade, 7, 500 + Seed); };
		const float Angle = Rand(0) * 2.0f * PI;
		const float Spread = Rand(1) * 14.0f;
		const FVector3f Base(FMath::Cos(Rand(2) * 2.0f * PI) * Spread, FMath::Sin(Rand(2) * 2.0f * PI) * Spread, 0.0f);
		const FVector3f Facing(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
		const FVector3f Side(-Facing.Y, Facing.X, 0.0f);
		const float Height = FMath::Lerp(32.0f, 68.0f, Rand(3));
		const float HalfWidth = FMath::Lerp(0.6f, 1.1f, Rand(4));
		const float Lean = FMath::Lerp(6.0f, 22.0f, Rand(5));
		const FVector3f Normal = (Facing * 0.6f + FVector3f(0, 0, 0.8f)).GetSafeNormal();

		TArray<FVertexInstanceID> Row[Segments + 1];
		for (int32 S = 0; S <= Segments; ++S)
		{
			const float T = static_cast<float>(S) / Segments;
			const FVector3f Center = Base + Facing * Lean * T * T + FVector3f(0, 0, Height * T);
			const float W = HalfWidth * (1.0f - T * 0.92f);
			for (const float Edge : { -1.0f, 1.0f })
			{
				const FVertexID Vertex = Description.CreateVertex();
				Positions[Vertex] = Center + Side * W * Edge;
				const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
				Normals[Instance] = Normal;
				Tangents[Instance] = Side;
				Signs[Instance] = 1.0f;
				Colors[Instance] = FVector4f(T, 0.0f, 0.0f, 1.0f);
				UVs.Set(Instance, 0, FVector2f(Edge * 0.5f + 0.5f, 1.0f - T));
				Row[S].Add(Instance);
			}
		}
		for (int32 S = 0; S < Segments; ++S)
		{
			Description.CreateTriangle(Group, TArray<FVertexInstanceID>{ Row[S][0], Row[S + 1][0], Row[S][1] });
			Description.CreateTriangle(Group, TArray<FVertexInstanceID>{ Row[S][1], Row[S + 1][0], Row[S + 1][1] });
		}
	}

	UStaticMesh* Mesh = NewObject<UStaticMesh>(this, TEXT("SM_GrassClump"));
	Mesh->GetStaticMaterials().Add(FStaticMaterial(GrassMaterial, TEXT("Grass"), TEXT("Grass")));
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = true;
	Params.bBuildSimpleCollision = false;
	Mesh->BuildFromMeshDescriptions({ &Description }, Params);
	return Mesh;
}

void ATerrainStreamer::UpdateGrass(const FVector& ViewLocation, double Deadline)
{
	const float Ground = FMath::Max(WorldGen::Height(ViewLocation.X, ViewLocation.Y), 0.0f);
	const bool bWanted = GrassMesh && ViewLocation.Z - Ground < GrassMaxViewHeight;
	const FIntPoint Center(FMath::FloorToInt(ViewLocation.X / GrassTileSize), FMath::FloorToInt(ViewLocation.Y / GrassTileSize));
	const int32 Radius = FMath::CeilToInt(GrassRadius / GrassTileSize);

	for (auto It = GrassTiles.CreateIterator(); It; ++It)
	{
		if (!bWanted || (It.Key() - Center).SizeSquared() > FMath::Square(Radius + 1))
		{
			It.Value()->ClearInstances();
			It.Value()->SetVisibility(false);
			GrassPool.Add(It.Value());
			It.RemoveCurrent();
		}
	}
	if (!bWanted)
	{
		return;
	}

	TArray<TPair<int32, FIntPoint>> Missing;
	for (int32 DY = -Radius; DY <= Radius; ++DY)
	{
		for (int32 DX = -Radius; DX <= Radius; ++DX)
		{
			const FIntPoint Tile = Center + FIntPoint(DX, DY);
			if (DX * DX + DY * DY <= Radius * Radius && !GrassTiles.Contains(Tile))
			{
				Missing.Emplace(DX * DX + DY * DY, Tile);
			}
		}
	}
	Missing.Sort([](const TPair<int32, FIntPoint>& A, const TPair<int32, FIntPoint>& B) { return A.Key < B.Key; });

	for (int32 i = 0; i < Missing.Num(); ++i)
	{
		if (i > 0 && FPlatformTime::Seconds() > Deadline)
		{
			break;
		}
		UHierarchicalInstancedStaticMeshComponent* Component = GrassPool.Num() > 0 ? GrassPool.Pop(EAllowShrinking::No) : nullptr;
		if (!Component)
		{
			Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
			Component->SetMobility(EComponentMobility::Movable);
			Component->SetStaticMesh(GrassMesh);
			Component->SetMaterial(0, GrassMaterial);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetCastShadow(false);
			Component->NumCustomDataFloats = 3;
			Component->SetCullDistances(GrassRadius * 0.75f, GrassRadius);
			Component->SetupAttachment(RootComponent);
			Component->RegisterComponent();
			AllParts.Add(Component);
		}
		Component->SetVisibility(true);
		BuildGrassTile(Missing[i].Value, Component);
		GrassTiles.Add(Missing[i].Value, Component);
	}
}

void ATerrainStreamer::BuildGrassTile(const FIntPoint& Tile, UHierarchicalInstancedStaticMeshComponent* Component) const
{
	// Sample the world on the terrain's own 5 m grid, then place each clump on the exact rendered triangle.
	const int32 Cells = FMath::RoundToInt(GrassTileSize / GridSpacing);
	const FVector2D Origin(Tile.X * GrassTileSize, Tile.Y * GrassTileSize);
	TArray<FWorldSample> Samples;
	Samples.SetNum((Cells + 1) * (Cells + 1));
	for (int32 Y = 0; Y <= Cells; ++Y)
	{
		for (int32 X = 0; X <= Cells; ++X)
		{
			Samples[Y * (Cells + 1) + X] = WorldGen::Sample(Origin.X + X * GridSpacing, Origin.Y + Y * GridSpacing);
		}
	}
	auto At = [&](int32 X, int32 Y) -> const FWorldSample& { return Samples[Y * (Cells + 1) + X]; };

	TArray<FTransform> Transforms;
	TArray<float> Data;
	const float ClumpSpacing = GrassTileSize / GrassClumpsPerSide;
	for (int32 GY = 0; GY < GrassClumpsPerSide; ++GY)
	{
		for (int32 GX = 0; GX < GrassClumpsPerSide; ++GX)
		{
			const int32 HX = Tile.X * GrassClumpsPerSide + GX;
			const int32 HY = Tile.Y * GrassClumpsPerSide + GY;
			const FVector2D P = Origin + FVector2D((GX + WorldGen::HashFloat(HX, HY, 601)) * ClumpSpacing, (GY + WorldGen::HashFloat(HX, HY, 602)) * ClumpSpacing);

			const float CX = (P.X - Origin.X) / GridSpacing;
			const float CY = (P.Y - Origin.Y) / GridSpacing;
			const int32 IX = FMath::Clamp(FMath::FloorToInt(CX), 0, Cells - 1);
			const int32 IY = FMath::Clamp(FMath::FloorToInt(CY), 0, Cells - 1);
			const float FX = CX - IX;
			const float FY = CY - IY;
			const FWorldSample& BL = At(IX, IY);
			const FWorldSample& BR = At(IX + 1, IY);
			const FWorldSample& TL = At(IX, IY + 1);
			const FWorldSample& TR = At(IX + 1, IY + 1);

			// Same triangle split as the terrain mesh (diagonal from bottom-right to top-left).
			const float Height = FX + FY <= 1.0f
				? BL.Height + FX * (BR.Height - BL.Height) + FY * (TL.Height - BL.Height)
				: TR.Height + (1.0f - FX) * (TL.Height - TR.Height) + (1.0f - FY) * (BR.Height - TR.Height);
			if (Height < 40.0f)
			{
				continue;
			}

			const float Slope = FMath::Max(FMath::Abs(BR.Height - BL.Height), FMath::Abs(TL.Height - BL.Height)) / GridSpacing;
			const FWorldSample& S = BL;
			const float Grassy = (1.0f - S.Sand) * (1.0f - FMath::Max(S.Rock, Smooth(0.5f, 0.9f, Slope))) * (1.0f - S.Snow) * (1.0f - 0.55f * S.Forest);
			if (WorldGen::HashFloat(HX, HY, 603) > Grassy * 0.95f)
			{
				continue;
			}

			const float Size = FMath::Lerp(0.7f, 1.3f, WorldGen::HashFloat(HX, HY, 604));
			const float Tall = FMath::Lerp(0.8f, 1.35f, WorldGen::HashFloat(HX, HY, 605)) * FMath::Lerp(1.0f, 0.7f, S.Dryness);
			Transforms.Add(FTransform(FRotator(0.0f, WorldGen::HashFloat(HX, HY, 606) * 360.0f, 0.0f), FVector(P.X, P.Y, Height - 3.0f), FVector(Size, Size, Size * Tall)));

			// Lush green to summer straw with dryness; darker under trees; slight per-clump variation.
			FLinearColor Tint = FMath::Lerp(FLinearColor(1.0f, 1.0f, 1.0f), FLinearColor(2.0f, 1.55f, 0.7f), S.Dryness) * FMath::Lerp(1.0f, 0.75f, S.Forest);
			Tint = Jitter(Tint, WorldGen::HashFloat(HX, HY, 607), 0.15f);
			Data.Append({ Tint.R, Tint.G, Tint.B });
		}
	}

	Component->ClearInstances();
	if (Transforms.Num() > 0)
	{
		Component->AddInstances(Transforms, false, true);
		for (int32 i = 0; i < Transforms.Num(); ++i)
		{
			Component->SetCustomData(i, TArrayView<const float>(&Data[i * 3], 3), false);
		}
		Component->MarkRenderStateDirty();
	}
}

void ATerrainStreamer::BuildTerrain(const FIntPoint& Coord, FChunk& Chunk, int32 Step, bool bWithCollision)
{
	if (!Chunk.Mesh)
	{
		Chunk.Mesh = AcquireMesh();
	}
	BuildSurface(Chunk.Mesh, FVector(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize(), 0.0), ChunkResolution / Step, GridSpacing * Step, bWithCollision);
	Chunk.Step = Step;
	Chunk.bHasCollision = bWithCollision;
}

void ATerrainStreamer::BuildSurface(UProceduralMeshComponent* Mesh, const FVector& Origin, int32 N, float Spacing, bool bWithCollision) const
{
	const int32 Side = N + 1;

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
	TArray<FVector2D> UV0;
	TArray<FVector2D> UV1;
	TArray<FColor> LayerWeights;
	TArray<int32> Triangles;
	const int32 NumSkirt = 4 * N;
	const int32 NumVerts = Side * Side + NumSkirt;
	Vertices.Reserve(NumVerts);
	Normals.Reserve(NumVerts);
	UV0.Reserve(NumVerts);
	UV1.Reserve(NumVerts);
	LayerWeights.Reserve(NumVerts);
	Triangles.Reserve(N * N * 6 + NumSkirt * 12);

	auto ToByte = [](float V) { return static_cast<uint8>(FMath::Clamp(V, 0.0f, 1.0f) * 255.0f); };
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

			// Material layer weights (see M_Terrain): R sand, G rock, B snow, A forest floor.
			const float Snow = S.Snow * Smooth(0.5f, 0.72f, Normal.Z);
			const float Steep = Smooth(0.82f, 0.6f, Normal.Z) * Smooth(200.0f, 800.0f, S.Height);
			const float Rock = FMath::Max(Steep, S.Rock) * (1.0f - Snow * 0.8f);
			LayerWeights.Add(FColor(ToByte(S.Sand * (1.0f - Rock)), ToByte(Rock), ToByte(Snow), ToByte(S.Forest)));
			UV1.Add(FVector2D(S.Dryness, S.Wetness));
			UV0.Add(FVector2D(X, Y));
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

	const float SkirtDepth = 6.0f * Spacing;
	const int32 SkirtStart = Vertices.Num();
	for (int32 i = 0; i < NumSkirt; ++i)
	{
		// Copy before adding: Add() may reallocate the array the reference points into.
		const int32 Top = Perimeter[i];
		const FVector Vertex = Vertices[Top] - FVector(0, 0, SkirtDepth);
		const FVector Normal = Normals[Top];
		const FColor Layer = LayerWeights[Top];
		const FVector2D A = UV0[Top];
		const FVector2D B = UV1[Top];
		Vertices.Add(Vertex);
		Normals.Add(Normal);
		LayerWeights.Add(Layer);
		UV0.Add(A);
		UV1.Add(B);
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

	Mesh->SetWorldLocation(Origin);
	const TArray<FVector2D> Empty;
	Mesh->CreateMeshSection(0, Vertices, Triangles, Normals, UV0, UV1, Empty, Empty, LayerWeights, TArray<FProcMeshTangent>(), bWithCollision);
	Mesh->SetMaterial(0, TerrainMaterial);
}

void ATerrainStreamer::FPropBatch::Add(EPropPart Part, const FVector& Center, const FRotator& Rotation, const FVector& SizeCm, const FLinearColor& Color, float Glow, ESurface Surface)
{
	Instances[Part].Add({ Center, Rotation, SizeCm });
	CustomData[Part].Append({ Color.R, Color.G, Color.B, Glow, static_cast<float>(Surface) });
}

void ATerrainStreamer::BuildProps(const FIntPoint& Coord, FChunk& Chunk, EProps Level)
{
	FPropBatch Batch;
	AddCities(Coord, Batch);
	AddLighthouses(Coord, Batch);
	AddVolcanoGlow(Coord, Batch);
	AddFloatingIslands(Coord, Batch);
	if (Level == EProps::Full)
	{
		AddVegetation(Coord, Batch);
		AddVillages(Coord, Batch);
		AddStoneCircles(Coord, Batch);
	}

	for (int32 Part = 0; Part < NumParts; ++Part)
	{
		UHierarchicalInstancedStaticMeshComponent*& Component = Chunk.Parts[Part];
		const TArray<FPropBatch::FInstance>& Instances = Batch.Instances[Part];
		if (Instances.Num() == 0)
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

		// Fit each mesh to the requested size and centre it, whatever its native size and pivot.
		const FBox Bounds = PartMeshes[Part]->GetBoundingBox();
		const FVector MeshSize = Bounds.GetSize().ComponentMax(FVector(1.0));
		TArray<FTransform> Transforms;
		Transforms.Reserve(Instances.Num());
		for (const FPropBatch::FInstance& Instance : Instances)
		{
			const FVector Scale = Instance.Size / MeshSize;
			const FVector Location = Instance.Center - Instance.Rotation.RotateVector(Bounds.GetCenter() * Scale);
			Transforms.Add(FTransform(Instance.Rotation, Location, Scale));
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
			Component->SetCustomData(i, TArrayView<const float>(&Data[i * 5], 5), false);
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
	GatherClearings(StoneCircleCell, StoneCircleRadius + 1500.0, [](int32 X, int32 Y, FVector& P) { return FindStoneCircle(X, Y, P); });
	GatherClearings(LighthouseCell, 3000.0, [](int32 X, int32 Y, FVector& P) { return FindLighthouse(X, Y, P); });

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

			const float ShrubChance = 0.03f + (S.Biome == EBiome::Grassland || S.Biome == EBiome::Savanna ? 0.05f : 0.0f) + (S.Biome == EBiome::Desert ? 0.02f : 0.0f);
			const float BoulderChance = 0.012f + S.Mountains * 0.05f + S.Rock * 0.06f + (S.Biome == EBiome::Desert ? 0.015f : 0.0f);
			const float Kind = WorldGen::HashFloat(CX, CY, SeedTreeKind);
			const float Size = FMath::Lerp(0.75f, 1.35f, WorldGen::HashFloat(CX, CY, SeedTreeSize));
			const float Tint = WorldGen::HashFloat(CX, CY, SeedTreeTint);
			const FRotator Spin(0.0f, Tint * 360.0f, 0.0f);
			const FVector Ground(PX, PY, S.Height);

			// Canopies are clusters of scanned foliage around the top of a trunk.
			auto Crown = [&](const FVector& Top, float Width, float Height, const FLinearColor& Leaves, int32 Clusters)
			{
				Batch.Add(Bush, Top, Spin, FVector(Width, Width, Height), Jitter(Leaves, Tint));
				for (int32 i = 1; i < Clusters; ++i)
				{
					const float A = (Tint + i / static_cast<float>(Clusters)) * 2.0f * PI;
					const FVector Offset(FMath::Cos(A) * Width * 0.3f, FMath::Sin(A) * Width * 0.3f, -Height * 0.15f);
					Batch.Add(Bush, Top + Offset, FRotator(0.0f, A * 57.0f, 0.0f), FVector(Width, Width, Height) * 0.7f, Jitter(Leaves, Kind));
				}
			};

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
				case EBiome::Savanna:
				{
					// Acacia: short trunk, flat spreading crown.
					const float H = 500.0f * Size;
					Batch.Add(Trunk, Ground + FVector(0, 0, H * 0.5f), Spin, FVector(50, 50, H), Bark, 0.0f, SurfConcrete);
					Crown(Ground + FVector(0, 0, H + 100.0f), 950.0f * Size, 260.0f * Size, LeafDry, 3);
					break;
				}
				case EBiome::Jungle:
				{
					// Tall emergent trees with broad, layered canopies.
					const float H = 1800.0f * Size;
					Batch.Add(Trunk, Ground + FVector(0, 0, H * 0.5f), Spin, FVector(100, 100, H), Bark, 0.0f, SurfConcrete);
					Crown(Ground + FVector(0, 0, H), 1200.0f * Size, 600.0f * Size, LeafJungle, 4);
					Crown(Ground + FVector(0, 0, 250.0f), 500.0f * Size, 400.0f * Size, LeafJungle, 1);
					break;
				}
				case EBiome::Taiga:
				case EBiome::Tundra:
				case EBiome::Snow:
				{
					// Conifer: foliage stacked narrower towards the top; frosted in the deep cold.
					const FLinearColor Needles = FMath::Lerp(LeafConifer, LeafFrost, Smooth(0.22f, 0.08f, S.Temperature));
					const float H = 1300.0f * Size;
					Batch.Add(Trunk, Ground + FVector(0, 0, H * 0.45f), Spin, FVector(45, 45, H * 0.9f), Bark, 0.0f, SurfConcrete);
					for (int32 Tier = 0; Tier < 4; ++Tier)
					{
						const float T = Tier / 3.0f;
						const float W = FMath::Lerp(450.0f, 150.0f, T) * Size;
						Batch.Add(Bush, Ground + FVector(0, 0, FMath::Lerp(0.3f, 0.92f, T) * H), FRotator(0, Tier * 80.0f + Tint * 360.0f, 0), FVector(W, W, H * 0.3f), Jitter(Needles, Tint));
					}
					break;
				}
				default:
				{
					// Broadleaf, a few already turning gold or rust, with the odd conifer mixed in.
					if (Kind < 0.18f)
					{
						const float H = 1200.0f * Size;
						Batch.Add(Trunk, Ground + FVector(0, 0, H * 0.45f), Spin, FVector(45, 45, H * 0.9f), Bark, 0.0f, SurfConcrete);
						for (int32 Tier = 0; Tier < 3; ++Tier)
						{
							const float W = FMath::Lerp(420.0f, 170.0f, Tier / 2.0f) * Size;
							Batch.Add(Bush, Ground + FVector(0, 0, FMath::Lerp(0.35f, 0.9f, Tier / 2.0f) * H), FRotator(0, Tier * 110.0f, 0), FVector(W, W, H * 0.35f), Jitter(LeafConifer, Tint));
						}
						break;
					}
					FLinearColor Leaves = LeafTemperate;
					if (Kind > 0.93f) Leaves = LeafGold;
					else if (Kind > 0.88f) Leaves = LeafRust;
					const float H = 450.0f * Size;
					Batch.Add(Trunk, Ground + FVector(0, 0, H * 0.5f), Spin, FVector(60, 60, H), Bark, 0.0f, SurfConcrete);
					Crown(Ground + FVector(0, 0, H + 250.0f * Size), 750.0f * Size, 650.0f * Size, Leaves, 3);
					break;
				}
				}
			}
			else if (Chance < S.TreeDensity + ShrubChance)
			{
				const FLinearColor Leaves = FMath::Lerp(LeafTemperate, LeafDry, S.Dryness);
				Batch.Add(Bush, Ground + FVector(0, 0, 60.0f * Size), Spin, FVector(220, 220, 160) * Size, Jitter(Leaves, Tint));
			}
			else if (Chance < S.TreeDensity + ShrubChance + BoulderChance)
			{
				const float B = FMath::Lerp(150.0f, 900.0f, Kind * Kind) * Size;
				const FLinearColor Stone = FMath::Lerp(White, FLinearColor(1.35f, 1.1f, 0.8f), S.Dryness);
				Batch.Add(Rock, Ground + FVector(0, 0, B * 0.2f), FRotator(Tint * 20.0f, Tint * 360.0f, Kind * 15.0f), FVector(B, B * 0.85f, B * 0.6f), Jitter(Stone, Tint, 0.1f));
			}
		}
	}
}

double ATerrainStreamer::VillageCellSize() { return VillageCell; }
double ATerrainStreamer::CityCellSize() { return CityCell; }
double ATerrainStreamer::IslandCellSize() { return IslandCell; }
double ATerrainStreamer::LighthouseCellSize() { return LighthouseCell; }
double ATerrainStreamer::StoneCircleCellSize() { return StoneCircleCell; }

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
	return S.Height >= 300.0f && S.Height <= 30000.0f && S.Mountains <= 0.3f && S.Volcano <= 0.0f && S.Biome != EBiome::Beach && S.Biome != EBiome::Snow;
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
	return S.Height >= 300.0f && S.Height <= 15000.0f && S.Mountains <= 0.15f && S.Volcano <= 0.0f && S.Biome != EBiome::Snow && S.Biome != EBiome::Jungle;
}

bool ATerrainStreamer::FindFloatingIsland(int32 CX, int32 CY, FVector& OutTop, float& OutRadius)
{
	FRandom Rand{ CX, CY, SeedIsland };
	if (Rand.Next() > 0.2f)
	{
		return false;
	}
	OutTop = FVector((CX + Rand.Next()) * IslandCell, (CY + Rand.Next()) * IslandCell, 0.0);
	OutTop.Z = FMath::Max(WorldGen::Height(OutTop.X, OutTop.Y), 0.0f) + Rand.Range(40000.0f, 100000.0f);
	OutRadius = Rand.Range(6000.0f, 24000.0f);
	return true;
}

bool ATerrainStreamer::FindLighthouse(int32 CX, int32 CY, FVector& OutBase)
{
	FRandom Rand{ CX, CY, SeedLighthouse };
	if (Rand.Next() > 0.6f)
	{
		return false;
	}
	OutBase = FVector((CX + Rand.Range(0.1f, 0.9f)) * LighthouseCell, (CY + Rand.Range(0.1f, 0.9f)) * LighthouseCell, 0.0);
	OutBase.Z = WorldGen::Height(OutBase.X, OutBase.Y);
	if (OutBase.Z < 300.0f || OutBase.Z > 3000.0f)
	{
		return false;
	}
	// Needs open sea close by.
	for (int32 i = 0; i < 8; ++i)
	{
		const float A = i * PI / 4.0f;
		if (WorldGen::Height(OutBase.X + FMath::Cos(A) * 20000.0, OutBase.Y + FMath::Sin(A) * 20000.0) < -300.0f)
		{
			return true;
		}
	}
	return false;
}

bool ATerrainStreamer::FindStoneCircle(int32 CX, int32 CY, FVector& OutCenter)
{
	FRandom Rand{ CX, CY, SeedStones };
	if (Rand.Next() > 0.25f)
	{
		return false;
	}
	OutCenter = FVector((CX + Rand.Range(0.2f, 0.8f)) * StoneCircleCell, (CY + Rand.Range(0.2f, 0.8f)) * StoneCircleCell, 0.0);
	const FWorldSample S = WorldGen::Sample(OutCenter.X, OutCenter.Y);
	OutCenter.Z = S.Height;
	return S.Height >= 500.0f && S.Mountains <= 0.3f && S.Volcano <= 0.0f &&
		(S.Biome == EBiome::Grassland || S.Biome == EBiome::Tundra || S.Biome == EBiome::Savanna);
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

			// Building style follows the climate: adobe in the heat, timber in the cold, brick and plaster elsewhere.
			const bool bAdobe = S.Biome == EBiome::Desert || S.Biome == EBiome::Savanna;
			const bool bTimber = S.Biome == EBiome::Taiga || S.Biome == EBiome::Tundra;
			const FLinearColor Plaster[] = { FLinearColor(1.6f, 1.5f, 1.3f), FLinearColor(1.7f, 1.65f, 1.55f), FLinearColor(1.5f, 1.3f, 1.0f) };
			const FLinearColor Roofs[] = { FLinearColor(1.2f, 0.55f, 0.4f), FLinearColor(0.7f, 0.7f, 0.75f), FLinearColor(0.9f, 0.6f, 0.45f) };

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
					// A stone church or temple at the heart of the village.
					const FLinearColor Stone = bAdobe ? FLinearColor(1.5f, 1.25f, 0.95f) : FLinearColor(1.3f, 1.28f, 1.22f);
					Batch.Add(Cube, FVector(Pos.X, Pos.Y, Ground + 650.0f), FRotator(0, Yaw, 0), FVector(900, 1500, 1500), Stone, 0.0f, bAdobe ? SurfConcrete : SurfRock);
					if (bAdobe)
					{
						Batch.Add(Sphere, FVector(Pos.X, Pos.Y, Ground + 1400.0f), FRotator::ZeroRotator, FVector(850, 850, 850), FLinearColor(1.7f, 1.6f, 1.45f), 0.0f, SurfConcrete);
					}
					else
					{
						Batch.Add(Cube, FVector(Pos.X, Pos.Y, Ground + 1400.0f), FRotator(0, Yaw, 45), FVector(1500, 900 / UE_SQRT_2 + 40, 900 / UE_SQRT_2 + 40), Roofs[1], 0.0f, SurfSlate);
						const FVector Tower = FRotator(0, Yaw, 0).RotateVector(FVector(0, 800, 0));
						Batch.Add(Cube, FVector(Pos.X, Pos.Y, Ground + 1200.0f) + Tower, FRotator(0, Yaw, 0), FVector(450, 450, 2500), Stone, 0.0f, SurfRock);
						Batch.Add(Cone, FVector(Pos.X, Pos.Y, Ground + 2450.0f + 500.0f) + Tower, FRotator(0, Yaw, 0), FVector(520, 520, 1000), Roofs[1], 0.0f, SurfSlate);
					}
					continue;
				}

				const float W = House.Range(700.0f, 1100.0f);
				const float D = House.Range(550.0f, 850.0f);
				const float H = House.Range(450.0f, 650.0f);
				FLinearColor Wall = Plaster[FMath::FloorToInt(House.Next() * 3) % 3];
				ESurface WallSurface = House.Next() < 0.4f ? SurfBrick : SurfConcrete;
				if (bTimber) { Wall = FLinearColor(0.5f, 0.36f, 0.25f); WallSurface = SurfConcrete; }
				if (bAdobe) { Wall = FLinearColor(1.5f, 1.2f, 0.85f); WallSurface = SurfConcrete; }
				if (WallSurface == SurfBrick) { Wall = White; }
				Batch.Add(Cube, FVector(Pos.X, Pos.Y, Ground + H * 0.5f - 50.0f), FRotator(0, Yaw, 0), FVector(W, D, H), Jitter(Wall, House.Next(), 0.06f), 0.0f, WallSurface);

				if (!bAdobe)
				{
					// Pitched roof: a box turned 45 degrees about the ridge, its diagonal spanning the house.
					const float S2 = D / UE_SQRT_2 + 40.0f;
					const FLinearColor Roof = bTimber ? FLinearColor(0.55f, 0.55f, 0.58f) : Roofs[FMath::FloorToInt(House.Next() * 3) % 3];
					Batch.Add(Cube, FVector(Pos.X, Pos.Y, Ground + H - 50.0f), FRotator(0, Yaw, 45), FVector(W + 60.0f, S2, S2), Roof, 0.0f, SurfSlate);
				}
			}
		}
	}
}

void ATerrainStreamer::AddStoneCircles(const FIntPoint& Coord, FPropBatch& Batch) const
{
	const FVector ChunkMin(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize(), 0.0);
	const int32 MinCX = FMath::FloorToInt((ChunkMin.X - StoneCircleRadius) / StoneCircleCell);
	const int32 MaxCX = FMath::FloorToInt((ChunkMin.X + ChunkWorldSize() + StoneCircleRadius) / StoneCircleCell);
	const int32 MinCY = FMath::FloorToInt((ChunkMin.Y - StoneCircleRadius) / StoneCircleCell);
	const int32 MaxCY = FMath::FloorToInt((ChunkMin.Y + ChunkWorldSize() + StoneCircleRadius) / StoneCircleCell);

	for (int32 CY = MinCY; CY <= MaxCY; ++CY)
	{
		for (int32 CX = MinCX; CX <= MaxCX; ++CX)
		{
			FVector Center;
			if (!FindStoneCircle(CX, CY, Center))
			{
				continue;
			}
			FRandom Rand{ CX, CY, SeedStones + 1 };
			const int32 Stones = 12 + FMath::FloorToInt(Rand.Next() * 8);
			const float Ring = Rand.Range(1200.0f, 1800.0f);
			const FLinearColor Stone(1.15f, 1.12f, 1.05f);
			for (int32 i = 0; i < Stones; ++i)
			{
				const float A = 2.0f * PI * i / Stones;
				const FVector Pos = Center + FVector(FMath::Cos(A) * Ring, FMath::Sin(A) * Ring, 0.0);
				if (!ChunkContains(Coord, Pos) || Rand.Next() < 0.12f)
				{
					continue; // a few have fallen or been taken
				}
				const float Ground = WorldGen::Height(Pos.X, Pos.Y);
				const float H = Rand.Range(350.0f, 550.0f);
				const FRotator Lean(Rand.Range(-6.0f, 6.0f), A * 57.2958f + 90.0f, Rand.Range(-6.0f, 6.0f));
				Batch.Add(Cube, FVector(Pos.X, Pos.Y, Ground + H * 0.45f), Lean, FVector(220, 110, H), Jitter(Stone, Rand.Next(), 0.1f), 0.0f, SurfRock);
				if (i % 2 == 0 && Rand.Next() < 0.6f)
				{
					// Lintel bridging to the next stone.
					const float A2 = A + PI / Stones;
					const FVector Mid = Center + FVector(FMath::Cos(A2) * Ring, FMath::Sin(A2) * Ring, 0.0);
					Batch.Add(Cube, FVector(Mid.X, Mid.Y, Ground + H * 0.9f + 50.0f), FRotator(0, A2 * 57.2958f + 90.0f, 0), FVector(2.0f * PI * Ring / Stones + 150.0f, 110, 90), Stone, 0.0f, SurfRock);
				}
			}
			if (ChunkContains(Coord, Center))
			{
				Batch.Add(Cube, FVector(Center.X, Center.Y, Center.Z + 50.0f), FRotator(0, Rand.Range(0.0f, 180.0f), 0), FVector(380, 160, 90), Stone, 0.0f, SurfRock);
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
					const bool bGlass = Tower.Next() < 0.45f;
					const FLinearColor Facade = bGlass ? FLinearColor(0.8f, 0.9f, 1.0f) : Jitter(FLinearColor(1.25f, 1.2f, 1.12f), Tower.Next(), 0.15f);
					const FRotator Rot(0, Tower.Range(-4.0f, 4.0f), 0);
					Batch.Add(Cube, FVector(Pos.X, Pos.Y, Ground + H * 0.5f - 200.0f), Rot, FVector(W, W * Tower.Range(0.7f, 1.0f), H), Facade, 0.0f, bGlass ? SurfGlass : SurfConcrete);
					// Rooftop plant room.
					Batch.Add(Cube, FVector(Pos.X, Pos.Y, Ground + H - 200.0f + 250.0f), Rot, FVector(W * 0.5f, W * 0.4f, 500.0f), FLinearColor(1.1f, 1.1f, 1.1f), 0.0f, SurfConcrete);
					if (H > 12000.0f)
					{
						Batch.Add(Sphere, FVector(Pos.X, Pos.Y, Ground + H + 350.0f), FRotator::ZeroRotator, FVector(120, 120, 120), FLinearColor(1.0f, 0.1f, 0.05f), 40.0f, SurfGlass);
					}
				}
			}
		}
	}
}

void ATerrainStreamer::AddLighthouses(const FIntPoint& Coord, FPropBatch& Batch) const
{
	const FVector ChunkMin(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize(), 0.0);
	for (int32 CY = FMath::FloorToInt(ChunkMin.Y / LighthouseCell); CY <= FMath::FloorToInt((ChunkMin.Y + ChunkWorldSize()) / LighthouseCell); ++CY)
	{
		for (int32 CX = FMath::FloorToInt(ChunkMin.X / LighthouseCell); CX <= FMath::FloorToInt((ChunkMin.X + ChunkWorldSize()) / LighthouseCell); ++CX)
		{
			FVector Base;
			if (!FindLighthouse(CX, CY, Base) || !ChunkContains(Coord, Base))
			{
				continue;
			}
			// White tower with red bands, glazed lantern room and a keeper's cottage.
			const FLinearColor Paint(1.8f, 1.8f, 1.75f);
			const FLinearColor Red(1.3f, 0.25f, 0.2f);
			const float H = 2600.0f;
			Batch.Add(Cylinder, Base + FVector(0, 0, H * 0.5f), FRotator::ZeroRotator, FVector(500, 500, H), Paint, 0.0f, SurfConcrete);
			Batch.Add(Cylinder, Base + FVector(0, 0, H * 0.35f), FRotator::ZeroRotator, FVector(510, 510, 400), Red, 0.0f, SurfConcrete);
			Batch.Add(Cylinder, Base + FVector(0, 0, H * 0.7f), FRotator::ZeroRotator, FVector(510, 510, 400), Red, 0.0f, SurfConcrete);
			Batch.Add(Cylinder, Base + FVector(0, 0, H + 20.0f), FRotator::ZeroRotator, FVector(620, 620, 60), Paint, 0.0f, SurfConcrete);
			Batch.Add(Cylinder, Base + FVector(0, 0, H + 220.0f), FRotator::ZeroRotator, FVector(360, 360, 360), White, 0.0f, SurfGlass);
			Batch.Add(Sphere, Base + FVector(0, 0, H + 220.0f), FRotator::ZeroRotator, FVector(160, 160, 160), FLinearColor(1.0f, 0.85f, 0.5f), 25.0f, SurfGlass);
			Batch.Add(Cone, Base + FVector(0, 0, H + 550.0f), FRotator::ZeroRotator, FVector(440, 440, 300), Red, 0.0f, SurfSlate);
			Batch.Add(Cube, Base + FVector(900, 0, 250.0f), FRotator::ZeroRotator, FVector(900, 600, 500), Paint, 0.0f, SurfConcrete);
		}
	}
}

void ATerrainStreamer::AddVolcanoGlow(const FIntPoint& Coord, FPropBatch& Batch) const
{
	const FVector ChunkMin(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize(), 0.0);
	const double Cell = WorldGen::VolcanoCellSize();
	for (int32 CY = FMath::FloorToInt(ChunkMin.Y / Cell); CY <= FMath::FloorToInt((ChunkMin.Y + ChunkWorldSize()) / Cell); ++CY)
	{
		for (int32 CX = FMath::FloorToInt(ChunkMin.X / Cell); CX <= FMath::FloorToInt((ChunkMin.X + ChunkWorldSize()) / Cell); ++CX)
		{
			FVector2D Center;
			float Radius, Peak;
			if (!WorldGen::FindVolcano(CX, CY, Center, Radius, Peak) || !ChunkContains(Coord, FVector(Center, 0.0)))
			{
				continue;
			}
			// Molten rock glowing in the crater.
			const float Floor = WorldGen::Height(Center.X, Center.Y);
			Batch.Add(Sphere, FVector(Center, Floor + 800.0f), FRotator::ZeroRotator, FVector(Radius * 0.12f, Radius * 0.12f, 2500.0f), FLinearColor(1.0f, 0.3f, 0.05f), 12.0f, SurfRock);
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

			// Rocky underside tapering to a point, a grassy cap, and a small wood on top.
			const float Depth = R * Rand.Range(1.2f, 2.0f);
			Batch.Add(Cone, Top - FVector(0, 0, Depth * 0.5f), FRotator(180, Rand.Range(0.0f, 360.0f), 0), FVector(R * 2.0f, R * 2.0f, Depth), White, 0.0f, SurfRock);
			Batch.Add(Sphere, Top, FRotator::ZeroRotator, FVector(R * 2.1f, R * 2.1f, R * 0.35f), White, 0.0f, SurfGrass);

			const int32 Trees = 8 + FMath::FloorToInt(Rand.Next() * 16);
			for (int32 i = 0; i < Trees; ++i)
			{
				const float A = Rand.Range(0.0f, 2.0f * PI);
				const float D = R * 0.7f * FMath::Sqrt(Rand.Next());
				const FVector Base = Top + FVector(FMath::Cos(A) * D, FMath::Sin(A) * D, R * 0.15f * (1.0f - D / R));
				const float Size = Rand.Range(0.8f, 1.5f);
				Batch.Add(Trunk, Base + FVector(0, 0, 250.0f * Size), FRotator::ZeroRotator, FVector(60, 60, 500) * Size, Bark, 0.0f, SurfConcrete);
				Batch.Add(Bush, Base + FVector(0, 0, 750.0f * Size), FRotator(0, A * 57.0f, 0), FVector(750, 750, 650) * Size, Jitter(LeafTemperate, Rand.Next()));
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
		Component->SetMaterial(0, PartMaterials[Part]);
		// Trunks, rocks and buildings are solid so you can weave between them; foliage stays soft.
		if (Part == Bush)
		{
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		else
		{
			Component->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
			Component->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		}
		Component->NumCustomDataFloats = 5;
		if (Part == Rock || Part == Bush || Part == Trunk)
		{
			Component->SetCullDistances(DetailCullDistance * 0.8f, DetailCullDistance);
		}
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
