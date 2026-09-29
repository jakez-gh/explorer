#include "Procedural/TerrainStreamer.h"
#include "Procedural/WorldGen.h"
#include "Procedural/RealPlace.h"
#include "Procedural/HouseGen.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "ProceduralMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
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
		SeedRuin = 120,
		SeedObelisk = 140,
		SeedWorldTree = 160,
		SeedSkyGate = 180,
		SeedWindFarm = 200,
	};

	constexpr double VillageCell = 250000.0;
	constexpr double VillageRadius = 20000.0;
	constexpr double CityCell = 900000.0;
	constexpr double CityRadius = 70000.0;
	constexpr double IslandCell = 900000.0;
	constexpr double LighthouseCell = 500000.0;
	constexpr double StoneCircleCell = 400000.0;
	constexpr double StoneCircleRadius = 2500.0;
	// Houses and their furniture are built to human scale, then enlarged so the world feels grand.
	constexpr float HouseScale = 1.0f;
	constexpr float TreeScale = 1.0f;
	constexpr double RuinCell = 600000.0;
	constexpr double RuinRadius = 6000.0;
	constexpr double ObeliskCell = 700000.0;
	constexpr double ObeliskRadius = 4500.0;
	constexpr double WorldTreeCell = 1500000.0;
	constexpr double WorldTreeRadius = 9000.0;
	constexpr double SkyGateCell = 700000.0;
	constexpr double SkyGateRadius = 14000.0;
	constexpr double WindFarmCell = 1200000.0;
	constexpr double WindFarmRadius = 16000.0;

	// Vegetation and rocks fade out beyond this distance; terrain colour carries forests further out.
	constexpr float DetailCullDistance = 150000.0f;

	// Far terrain tiles: 4x4 chunks each, dropped slightly so nearer, finer terrain always wins where they overlap.
	constexpr int32 FarTileChunks = 4;
	constexpr int32 FarTileResolution = 32;
	constexpr float FarTileDrop = 2500.0f;

	// Grass tiles align with the 5 m terrain grid so blades sit exactly on the rendered surface.
	constexpr double GrassTileSize = 2000.0;
	constexpr int32 GrassClumpsPerSide = 22;

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

	// Bark tints (multiply M_Bark's brown).
	const FLinearColor BarkWarm(1.0f, 0.95f, 0.85f);
	const FLinearColor BarkDark(0.8f, 0.72f, 0.65f);
	const FLinearColor BarkPale(1.5f, 1.45f, 1.3f);
	const FLinearColor BarkBirch(4.2f, 4.2f, 4.0f);

	/** Builds tapered tubes (trunk and branches) into a mesh description. */
	struct FTubeBuilder
	{
		FMeshDescription& Desc;
		FPolygonGroupID Group;
		TVertexAttributesRef<FVector3f> Positions;
		TVertexInstanceAttributesRef<FVector3f> Normals;
		TVertexInstanceAttributesRef<FVector3f> Tangents;
		TVertexInstanceAttributesRef<float> Signs;
		TVertexInstanceAttributesRef<FVector4f> Colors;
		TVertexInstanceAttributesRef<FVector2f> UVs;
		FVector4f Color = FVector4f(0, 0, 0, 1);

		void Tube(const TArray<FVector3f>& Points, const TArray<float>& Radii, int32 Sides)
		{
			const int32 N = Points.Num();
			TArray<TArray<FVertexInstanceID>> Rings;
			FVector3f PrevSide = FVector3f::ZeroVector;
			float V = 0.0f;
			for (int32 i = 0; i < N; ++i)
			{
				const FVector3f Along = (Points[FMath::Min(i + 1, N - 1)] - Points[FMath::Max(i - 1, 0)]).GetSafeNormal();
				FVector3f Side;
				if (i == 0)
				{
					const FVector3f Up = FMath::Abs(Along.Z) < 0.95f ? FVector3f(0, 0, 1) : FVector3f(1, 0, 0);
					Side = FVector3f::CrossProduct(Along, Up).GetSafeNormal();
				}
				else
				{
					// Parallel transport keeps the rings from twisting.
					Side = (PrevSide - Along * FVector3f::DotProduct(PrevSide, Along)).GetSafeNormal();
					V += FVector3f::Distance(Points[i], Points[i - 1]) / 150.0f;
				}
				PrevSide = Side;
				const FVector3f Binormal = FVector3f::CrossProduct(Along, Side);
				TArray<FVertexInstanceID>& Ring = Rings.AddDefaulted_GetRef();
				for (int32 K = 0; K <= Sides; ++K)
				{
					const float A = 2.0f * PI * K / Sides;
					const FVector3f Dir = Side * FMath::Cos(A) + Binormal * FMath::Sin(A);
					const FVertexID Vertex = Desc.CreateVertex();
					Positions[Vertex] = Points[i] + Dir * Radii[i];
					const FVertexInstanceID Instance = Desc.CreateVertexInstance(Vertex);
					Normals[Instance] = Dir;
					Tangents[Instance] = Along;
					Signs[Instance] = 1.0f;
					Colors[Instance] = Color;
					UVs.Set(Instance, 0, FVector2f(2.0f * K / Sides, V));
					Ring.Add(Instance);
				}
			}
			for (int32 i = 0; i + 1 < N; ++i)
			{
				for (int32 K = 0; K < Sides; ++K)
				{
					Desc.CreateTriangle(Group, TArray<FVertexInstanceID>{ Rings[i][K], Rings[i + 1][K], Rings[i][K + 1] });
					Desc.CreateTriangle(Group, TArray<FVertexInstanceID>{ Rings[i][K + 1], Rings[i + 1][K], Rings[i + 1][K + 1] });
				}
			}
		}
	};
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
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BarkMat(TEXT("/Game/Explorer/Materials/M_Bark.M_Bark"));
	BarkMaterial = BarkMat.Object;
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> PathMat(TEXT("/Game/Explorer/Materials/M_Path.M_Path"));
	PathMaterial = PathMat.Object;
	PartMeshes = { CylinderMesh.Object, ConeMesh.Object, SphereMesh.Object, CubeMesh.Object, RockMesh.Object, BushMesh.Object, CylinderMesh.Object };
	PartMaterials = { PropMat.Object, PropMat.Object, PropMat.Object, PropMat.Object, RockMat.Object, FoliageMat.Object, PropMat.Object };
	// Tree meshes are generated in BeginPlay.
	PartMeshes.SetNum(NumParts);
	PartMaterials.SetNum(NumParts);
	for (int32 Part = Tree0; Part < Scanned0; ++Part)
	{
		PartMaterials[Part] = BarkMaterial;
	}
	// Scanned trees keep their own materials (null here means "don't override").
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> IslandMat(TEXT("/Game/Explorer/Materials/M_Island.M_Island"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BarkFarMat(TEXT("/Game/Explorer/Materials/M_BarkFar.M_BarkFar"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> FoliageFarMat(TEXT("/Game/Explorer/Materials/M_FoliageFar.M_FoliageFar"));
	PartMeshes[IslandBush] = BushMesh.Object;
	PartMaterials[IslandBush] = FoliageFarMat.Object;
	PartMaterials[IslandTree] = BarkFarMat.Object;
	for (int32 Part = Island0; Part < Island0 + NumIslandVariants; ++Part)
	{
		PartMaterials[Part] = IslandMat.Object;
	}

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
	RealPlace::Load();
	GrassMesh = CreateGrassClumpMesh();
	for (int32 Variant = 0; Variant < NumTreeVariants; ++Variant)
	{
		PartMeshes[Tree0 + Variant] = CreateTreeMesh(static_cast<ETreeVariant>(Variant), TreeTemplates[Variant]);
	}
	PartMeshes[IslandTree] = PartMeshes[Tree0 + TreeBroadleafA];

	// Photoscanned Megascans trees (free Fab packs, not in git; see CLAUDE.md). Missing packs just
	// leave the generated broadleaf trees in place.
	static const TCHAR* ScannedPaths[NumScannedTrees] = {
		TEXT("EuropeanBeech/Geometry/SimpleWind/SM_EuropeanBeech_Forest_01"),
		TEXT("EuropeanBeech/Geometry/SimpleWind/SM_EuropeanBeech_Forest_02"),
		TEXT("EuropeanBeech/Geometry/SimpleWind/SM_EuropeanBeech_Forest_03"),
		TEXT("EuropeanBeech/Geometry/SimpleWind/SM_EuropeanBeech_Forest_04"),
		TEXT("EuropeanBeech/Geometry/SimpleWind/SM_EuropeanBeech_Forest_05"),
		TEXT("EuropeanBeech/Geometry/SimpleWind/SM_EuropeanBeech_Forest_06"),
		TEXT("EuropeanBeech/Geometry/SimpleWind/SM_EuropeanBeech_Forest_07"),
		TEXT("EuropeanBeech/Geometry/SimpleWind/SM_EuropeanBeech_Forest_08"),
		TEXT("EuropeanBeech/Geometry/SimpleWind/SM_EuropeanBeech_Field_01"),
		TEXT("NorwayMaple/Geometry/SimpleWind/SM_NorwayMaple_Forest_01"),
		TEXT("NorwayMaple/Geometry/SimpleWind/SM_NorwayMaple_Forest_02"),
		TEXT("NorwayMaple/Geometry/SimpleWind/SM_NorwayMaple_Forest_03"),
		TEXT("NorwayMaple/Geometry/SimpleWind/SM_NorwayMaple_Forest_04"),
		TEXT("NorwayMaple/Geometry/SimpleWind/SM_NorwayMaple_Forest_05"),
		TEXT("NorwayMaple/Geometry/SimpleWind/SM_NorwayMaple_Forest_06"),
		TEXT("NorwayMaple/Geometry/SimpleWind/SM_NorwayMaple_Forest_07"),
		TEXT("NorwayMaple/Geometry/SimpleWind/SM_NorwayMaple_Field_01"),
		TEXT("NorwayMaple/Geometry/SimpleWind/SM_NorwayMaple_Field_02"),
	};
	NumLoadedScanned = 0;
	for (int32 i = 0; i < NumScannedTrees; ++i)
	{
		const FString Name = FPaths::GetBaseFilename(ScannedPaths[i]);
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/%s.%s"), ScannedPaths[i], *Name), nullptr, LOAD_Quiet | LOAD_NoWarn);
		if (Mesh)
		{
			PartMeshes[Scanned0 + NumLoadedScanned] = Mesh;
			ScannedTreeHeight[NumLoadedScanned] = Mesh->GetBoundingBox().GetSize().Z;
			++NumLoadedScanned;
		}
	}
	MatureSlots.Reset();
	YoungSlots.Reset();
	for (int32 i = 0; i < NumLoadedScanned; ++i)
	{
		(ScannedTreeHeight[i] >= 1800.0f ? MatureSlots : YoungSlots).Add(i);
	}
	if (MatureSlots.Num() == 0)
	{
		MatureSlots = YoungSlots;
	}
	UE_LOG(LogTemp, Log, TEXT("Loaded %d scanned tree meshes (%d mature, %d young)"), NumLoadedScanned, MatureSlots.Num(), YoungSlots.Num());
	for (int32 Variant = 0; Variant < NumIslandVariants; ++Variant)
	{
		PartMeshes[Island0 + Variant] = CreateIslandMesh(Variant);
	}
	// House surfaces: boxes with photoscanned building materials.
	{
		static const TCHAR* Names[] = { TEXT("ExteriorWall"), TEXT("BrickWall"), TEXT("TimberWall"), TEXT("InteriorWall"), TEXT("PlankFloor"),
			TEXT("TileFloor"), TEXT("Stone"), TEXT("ClayRoof"), TEXT("SlateRoof"), TEXT("Wood"), TEXT("Carpet") };
		static_assert(UE_ARRAY_COUNT(Names) + 1 == static_cast<int32>(HouseGen::ESurface::Count), "one material per surface (plus glass)");
		for (int32 i = 0; i < static_cast<int32>(HouseGen::ESurface::Count); ++i)
		{
			PartMeshes[BuildSurf0 + i] = PartMeshes[Cube];
			const FString Path = i < UE_ARRAY_COUNT(Names)
				? FString::Printf(TEXT("/Game/Explorer/Materials/Building/MI_%s.MI_%s"), Names[i], Names[i])
				: FString(TEXT("/Game/Explorer/Materials/M_Glass.M_Glass"));
			UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_Quiet | LOAD_NoWarn);
			PartMaterials[BuildSurf0 + i] = Material ? Material : PartMaterials[Cube].Get();
		}
	}

	// Furniture: CC0 scanned models imported by Tools/import_assets.py. Each folder may hold several
	// meshes (parts of a set); take the largest. Missing models fall back to a small box.
	{
		static const TCHAR* Models[] = { TEXT("Sofa_01"), TEXT("ArmChair_01"), TEXT("CoffeeTable_01"), TEXT("wooden_bookshelf_worn"),
			TEXT("dining_table"), TEXT("dining_chair_02"), TEXT("electric_stove"), TEXT("painted_wooden_cabinet"), TEXT("vintage_cabinet_01"),
			TEXT("GothicBed_01"), TEXT("painted_wooden_nightstand"), TEXT("vintage_wooden_drawer_01"), TEXT("ornate_mirror_01"), TEXT("Shelf_01"),
			TEXT("wall_clock"), TEXT("scandinavian_masonry_heater"), TEXT("modern_ceiling_lamp_01") };
		static_assert(UE_ARRAY_COUNT(Models) == static_cast<int32>(HouseGen::EFurniture::Count), "one model per furniture type");
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		// The registry scans in the background in -game; wait for these folders.
		Registry.ScanPathsSynchronous({ TEXT("/Game/PolyHaven/Models") }, true);
		for (int32 i = 0; i < UE_ARRAY_COUNT(Models); ++i)
		{
			TArray<FAssetData> Assets;
			Registry.GetAssetsByPath(FName(*FString::Printf(TEXT("/Game/PolyHaven/Models/%s"), Models[i])), Assets, true);
			UStaticMesh* Best = nullptr;
			double BestSize = -1.0;
			for (const FAssetData& Asset : Assets)
			{
				if (Asset.IsInstanceOf(UStaticMesh::StaticClass()))
				{
					UStaticMesh* Mesh = Cast<UStaticMesh>(Asset.GetAsset());
					const double Size = Mesh ? Mesh->GetBoundingBox().GetVolume() : -1.0;
					if (Size > BestSize)
					{
						Best = Mesh;
						BestSize = Size;
					}
				}
			}
			UE_LOG(LogTemp, Display, TEXT("Furniture %s: %s (%d assets)"), Models[i], Best ? *Best->GetName() : TEXT("MISSING"), Assets.Num());
			PartMeshes[Furniture0 + i] = Best ? Best : PartMeshes[Cube].Get();
			PartMaterials[Furniture0 + i] = Best ? nullptr : PartMaterials[Cube].Get();
		}
	}

	PartBounds.SetNum(NumParts);
	for (int32 Part = 0; Part < NumParts; ++Part)
	{
		PartBounds[Part] = PartMeshes[Part] ? PartMeshes[Part]->GetBoundingBox() : FBox(FVector(-50.0), FVector(50.0));
	}
}

float ATerrainStreamer::IslandTopHeight(int32 Variant, float X, float Y)
{
	// Rolling lumps, a raised knoll off-centre, and a rim that rounds off at the edge.
	const FVector2D Offset(Variant * 17.3f, Variant * -9.1f);
	const float R = FMath::Sqrt(X * X + Y * Y) / 100.0f;
	const float Lumps = FMath::PerlinNoise2D(FVector2D(X, Y) / 45.0f + Offset) * 4.0f + FMath::PerlinNoise2D(FVector2D(X, Y) / 14.0f + Offset * 2.0f) * 1.2f;
	const FVector2D Knoll(FMath::Cos(Variant * 2.1f) * 35.0f, FMath::Sin(Variant * 2.1f) * 35.0f);
	const float Hill = 9.0f * FMath::Exp(-FVector2D::DistSquared(FVector2D(X, Y), Knoll) / 900.0f);
	const float Rim = -6.0f * FMath::Pow(FMath::Clamp(R, 0.0f, 1.0f), 4.0f);
	return Lumps + Hill + Rim;
}

UStaticMesh* ATerrainStreamer::CreateIslandMesh(int32 Variant)
{
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
	Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Island");

	// A polar grid: rings from the centre of the top out to the rim, then down the underside to the tip.
	constexpr int32 Segments = 72;
	constexpr int32 TopRings = 14;
	constexpr int32 UnderRings = 22;
	const FVector2D Offset(Variant * 11.7f, Variant * 5.3f);
	const float Depth = 45.0f + Variant * 6.0f;
	auto EdgeRadius = [&](float Angle)
	{
		// Irregular outline, not a circle.
		return 100.0f * (1.0f + 0.16f * FMath::PerlinNoise1D(Angle * 1.3f + Variant * 3.0f) + 0.06f * FMath::PerlinNoise1D(Angle * 5.0f + Variant));
	};

	// Vertex colour: R = earth (0 grass, 1 hanging soil), G = exposed rock, B = height along underside.
	struct FRing { TArray<FVector3f> P; TArray<FVector4f> C; };
	TArray<FRing> Rings;
	for (int32 i = 0; i <= TopRings; ++i)
	{
		FRing& Ring = Rings.AddDefaulted_GetRef();
		const float T = static_cast<float>(i) / TopRings;
		for (int32 s = 0; s <= Segments; ++s)
		{
			const float A = 2.0f * PI * s / Segments;
			const float R = EdgeRadius(A) * T;
			const float X = FMath::Cos(A) * R, Y = FMath::Sin(A) * R;
			const float Outcrop = FMath::Max(0.0f, FMath::PerlinNoise2D(FVector2D(X, Y) / 20.0f + Offset * 3.0f) - 0.35f);
			Ring.P.Add(FVector3f(X, Y, IslandTopHeight(Variant, X, Y) + Outcrop * 12.0f));
			Ring.C.Add(FVector4f(FMath::Pow(T, 12.0f) * 0.8f, FMath::Clamp(Outcrop * 4.0f, 0.0f, 1.0f), 0.0f, 1.0f));
		}
	}
	for (int32 i = 1; i <= UnderRings; ++i)
	{
		FRing& Ring = Rings.AddDefaulted_GetRef();
		const float T = static_cast<float>(i) / UnderRings; // 0 at the rim, 1 at the tip
		for (int32 s = 0; s <= Segments; ++s)
		{
			const float A = 2.0f * PI * s / Segments;
			// A broad, bulging body of earth that stays wide, then gathers into several hanging lobes of
			// soil, roots and rock, rather than a single cone.
			// A broad, shallow bowl of earth; separate hanging lobes are added below it.
			const float Taper = FMath::Pow(1.0f - T, 0.4f);
			const float Bulge = 1.0f + 0.1f * FMath::Sin(T * PI) + 0.12f * FMath::PerlinNoise2D(FVector2D(A * 2.0f, T * 4.0f) + Offset);
			const float R = EdgeRadius(A) * Taper * Bulge * (i == 1 ? 0.97f : 1.0f);
			const float Z = -Depth * FMath::Pow(T, 0.8f) * (1.0f + 0.25f * FMath::PerlinNoise2D(FVector2D(A * 3.0f, T * 3.0f) - Offset)) - 4.0f * FMath::Pow(1.0f - T, 3.0f);
			const float Rock = FMath::Clamp(FMath::PerlinNoise2D(FVector2D(A * 3.0f, T * 6.0f) + Offset * 2.0f) * 2.0f + T - 0.4f, 0.0f, 1.0f);
			Ring.P.Add(FVector3f(FMath::Cos(A) * R, FMath::Sin(A) * R, Z));
			Ring.C.Add(FVector4f(1.0f, Rock, T, 1.0f));
		}
	}

	// Vertices, with normals from neighbouring points on the grid.
	TArray<TArray<FVertexInstanceID>> Ids;
	for (int32 i = 0; i < Rings.Num(); ++i)
	{
		TArray<FVertexInstanceID>& Row = Ids.AddDefaulted_GetRef();
		for (int32 s = 0; s <= Segments; ++s)
		{
			const FVector3f& P = Rings[i].P[s];
			const FVector3f Across = Rings[i].P[(s + 1) % Segments] - Rings[i].P[(s + Segments - 1) % Segments];
			const FVector3f Along = Rings[FMath::Min(i + 1, Rings.Num() - 1)].P[s] - Rings[FMath::Max(i - 1, 0)].P[s];
			FVector3f N = FVector3f::CrossProduct(Across, Along).GetSafeNormal();
			if (i <= TopRings ? N.Z < 0.0f : FVector3f::DotProduct(N, FVector3f(P.X, P.Y, 0.0f)) < 0.0f)
			{
				N = -N;
			}
			if (i == 0)
			{
				N = FVector3f(0, 0, 1);
			}
			const FVertexID V = Description.CreateVertex();
			Positions[V] = P;
			const FVertexInstanceID I = Description.CreateVertexInstance(V);
			Normals[I] = N;
			Tangents[I] = Across.GetSafeNormal();
			Signs[I] = 1.0f;
			Colors[I] = Rings[i].C[s];
			UVs.Set(I, 0, FVector2f(static_cast<float>(s) / Segments, static_cast<float>(i) / Rings.Num()));
			Row.Add(I);
		}
	}
	// Wind each triangle so its front face agrees with the surface normal (in Unreal's convention the
	// front face is the one for which the cross product of the edges points away from the normal).
	auto Triangle = [&](FVertexInstanceID A, FVertexInstanceID B, FVertexInstanceID C)
	{
		const FVector3f PA = Positions[Description.GetVertexInstanceVertex(A)];
		const FVector3f PB = Positions[Description.GetVertexInstanceVertex(B)];
		const FVector3f PC = Positions[Description.GetVertexInstanceVertex(C)];
		const FVector3f N = Normals[A] + Normals[B] + Normals[C];
		if (FVector3f::DotProduct(FVector3f::CrossProduct(PB - PA, PC - PA), N) > 0.0f)
		{
			Swap(B, C);
		}
		Description.CreateTriangle(Group, TArray<FVertexInstanceID>{ A, B, C });
	};
	for (int32 i = 0; i + 1 < Rings.Num(); ++i)
	{
		for (int32 s = 0; s < Segments; ++s)
		{
			Triangle(Ids[i][s], Ids[i + 1][s], Ids[i][s + 1]);
			Triangle(Ids[i][s + 1], Ids[i + 1][s], Ids[i + 1][s + 1]);
		}
	}

	// Hanging lobes of soil and rock beneath the bowl, of varied length, each tapering to a drip.
	FTubeBuilder B{ Description, Group, Positions, Normals, Tangents, Signs, Colors, UVs };
	const int32 NumLobes = 7 + Variant * 2;
	for (int32 k = 0; k < NumLobes; ++k)
	{
		auto H = [&](uint32 Seed) { return WorldGen::HashFloat(Variant, k, 1200 + Seed); };
		const float A = H(0) * 2.0f * PI;
		const float Dist = (k == 0 ? 0.0f : FMath::Sqrt(H(1)) * 0.62f);
		const float BaseR = FMath::Lerp(14.0f, 32.0f, H(2)) * (1.0f - Dist * 0.6f);
		const float Length = FMath::Lerp(45.0f, 150.0f, H(3)) * (1.0f - Dist * 0.5f);
		const FVector3f Start(FMath::Cos(A) * Dist * 100.0f, FMath::Sin(A) * Dist * 100.0f, -Depth * FMath::Pow(1.0f - Dist, 0.8f) * 0.6f);
		const FVector3f Drift(H(4) * 16.0f - 8.0f, H(5) * 16.0f - 8.0f, 0.0f);
		TArray<FVector3f> Points;
		TArray<float> Radii;
		for (int32 j = 0; j <= 8; ++j)
		{
			const float T = j / 8.0f;
			Points.Add(Start + Drift * T * T + FVector3f(FMath::PerlinNoise1D(T * 3.0f + k) * 5.0f, FMath::PerlinNoise1D(T * 3.0f - k) * 5.0f, -Length * T));
			Radii.Add(FMath::Max(BaseR * FMath::Pow(1.0f - T, 1.3f) * (1.0f + 0.2f * FMath::PerlinNoise1D(T * 6.0f + k)), 0.5f));
		}
		B.Color = FVector4f(1.0f, H(6) < 0.4f ? 0.8f : 0.15f, 0.6f, 1.0f);
		B.Tube(Points, Radii, 10);
	}

	UStaticMesh* Mesh = NewObject<UStaticMesh>(this, *FString::Printf(TEXT("SM_Island%d"), Variant));
	Mesh->GetStaticMaterials().Add(FStaticMaterial(PartMaterials[Island0 + Variant], TEXT("Island"), TEXT("Island")));
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = true;
	Params.bBuildSimpleCollision = false;
	Mesh->BuildFromMeshDescriptions({ &Description }, Params);
	return Mesh;
}

UStaticMesh* ATerrainStreamer::CreateTreeMesh(ETreeVariant Variant, FTreeTemplate& Out)
{
	FMeshDescription Description;
	FStaticMeshAttributes Attributes(Description);
	Attributes.Register();
	Attributes.GetVertexInstanceUVs().SetNumChannels(1);
	FTubeBuilder B{ Description, Description.CreatePolygonGroup(), Attributes.GetVertexPositions(), Attributes.GetVertexInstanceNormals(),
		Attributes.GetVertexInstanceTangents(), Attributes.GetVertexInstanceBinormalSigns(), Attributes.GetVertexInstanceColors(), Attributes.GetVertexInstanceUVs() };
	Attributes.GetPolygonGroupMaterialSlotNames()[B.Group] = TEXT("Bark");

	int32 Counter = 0;
	auto Rand = [&]() { return WorldGen::HashFloat(Variant, Counter++, 800); };
	auto Range = [&](float Min, float Max) { return FMath::Lerp(Min, Max, Rand()); };

	// Shape parameters per kind.
	float Height = 800.0f, Radius = 30.0f, Lean = 30.0f, Flare = 1.8f;
	switch (Variant)
	{
	case TreeConiferA: case TreeConiferB: Height = Range(1300.0f, 1700.0f); Radius = 26.0f; Lean = 10.0f; Flare = 1.5f; break;
	case TreeAcacia: Height = 560.0f; Radius = 24.0f; Lean = 60.0f; Flare = 1.4f; break;
	case TreeJungle: Height = 2000.0f; Radius = 48.0f; Lean = 25.0f; Flare = 3.0f; break;
	case TreeBirch: Height = 950.0f; Radius = 15.0f; Lean = 45.0f; Flare = 1.4f; break;
	default: Height = Range(700.0f, 950.0f); Radius = Range(26.0f, 36.0f); Lean = Range(20.0f, 55.0f); Flare = 1.9f; break;
	}
	const float TrunkTop = Variant == TreeAcacia ? Height * 0.45f : Height;
	const float LeanAngle = Range(0.0f, 2.0f * PI);
	const FVector3f LeanDir(FMath::Cos(LeanAngle), FMath::Sin(LeanAngle), 0.0f);

	// Trunk axis and radius at a height: a gentle curve, tapering, flaring into roots at the base.
	auto Axis = [&](float Z) { const float T = Z / Height; return LeanDir * Lean * FMath::Sin(T * PI * 0.8f) + FVector3f(0, 0, Z); };
	auto TrunkRadius = [&](float Z)
	{
		const float T = FMath::Clamp(Z / TrunkTop, 0.0f, 1.0f);
		const float Taper = Radius * FMath::Lerp(1.0f, Variant == TreeAcacia ? 0.55f : 0.08f, FMath::Pow(T, 0.8f));
		return Taper + Radius * (Flare - 1.0f) * FMath::Exp(-FMath::Max(Z, 0.0f) / 45.0f);
	};

	TArray<FVector3f> Points;
	TArray<float> Radii;
	for (int32 i = 0; i <= 12; ++i)
	{
		const float Z = -40.0f + (TrunkTop + 40.0f) * i / 12.0f;
		Points.Add(Axis(Z));
		Radii.Add(i == 12 ? 1.0f : TrunkRadius(Z));
	}
	B.Tube(Points, Radii, 10);

	// Root buttresses spreading into the ground.
	const int32 Roots = Variant == TreeJungle ? 6 : 4;
	for (int32 r = 0; r < Roots; ++r)
	{
		const float A = (r + Range(-0.2f, 0.2f)) * 2.0f * PI / Roots;
		const FVector3f Dir(FMath::Cos(A), FMath::Sin(A), 0.0f);
		const float Reach = Radius * Flare * Range(2.0f, 3.0f);
		Points = { FVector3f(0, 0, Radius * 1.4f), Dir * Reach * 0.35f + FVector3f(0, 0, Radius * 0.4f), Dir * Reach + FVector3f(0, 0, -Radius * 0.4f) };
		Radii = { Radius * 0.5f, Radius * 0.35f, 2.0f };
		B.Tube(Points, Radii, 6);
	}

	// A branch from the trunk (starting inside it, so it's joined) curving out and up; its tip carries foliage.
	auto Branch = [&](const FVector3f& Start, const FVector3f& Dir, float Length, float StartRadius, float Rise, float TipSize, bool bMidFoliage)
	{
		TArray<FVector3f> BP;
		TArray<float> BR;
		for (int32 i = 0; i <= 5; ++i)
		{
			const float T = i / 5.0f;
			BP.Add(Start + Dir * Length * T + FVector3f(0, 0, Rise * Length * T * T));
			BR.Add(FMath::Lerp(StartRadius, 1.5f, T));
		}
		B.Tube(BP, BR, 6);
		Out.Tips.Add({ FVector(BP.Last()), TipSize });
		if (bMidFoliage)
		{
			Out.Tips.Add({ FVector(BP[3]), TipSize * 0.75f });
		}
		return BP;
	};

	switch (Variant)
	{
	case TreeConiferA:
	case TreeConiferB:
	{
		// Whorls of short, drooping branches, longest at the bottom.
		const int32 Count = 11;
		for (int32 i = 0; i < Count; ++i)
		{
			const float T = FMath::Lerp(0.2f, 0.9f, i / static_cast<float>(Count - 1));
			const float Z = T * Height;
			const float A = i * 2.4f + Range(-0.3f, 0.3f);
			const FVector3f Dir = FVector3f(FMath::Cos(A), FMath::Sin(A), -0.2f).GetSafeNormal();
			const float Length = (1.0f - T) * Height * 0.34f + 60.0f;
			Branch(Axis(Z), Dir, Length, FMath::Max(TrunkRadius(Z) * 0.35f, 3.0f), -0.1f, Length * 1.8f + 60.0f, true);
		}
		Out.Tips.Add({ FVector(Axis(Height * 0.95f)), 170.0f });
		break;
	}
	case TreeAcacia:
	{
		// The trunk forks into limbs that spread up and out to a flat top.
		const int32 Limbs = 4;
		for (int32 i = 0; i < Limbs; ++i)
		{
			const float A = (i + Range(-0.2f, 0.2f)) * 2.0f * PI / Limbs;
			const FVector3f Dir = FVector3f(FMath::Cos(A), FMath::Sin(A), 1.0f).GetSafeNormal();
			Branch(Axis(TrunkTop - 20.0f), Dir, Range(450.0f, 600.0f), Radius * 0.5f, 0.05f, 520.0f, true);
		}
		break;
	}
	case TreeJungle:
	{
		const int32 Limbs = 5;
		for (int32 i = 0; i < Limbs; ++i)
		{
			const float Z = Height * Range(0.78f, 0.9f);
			const float A = (i + Range(-0.2f, 0.2f)) * 2.0f * PI / Limbs;
			const FVector3f Dir = FVector3f(FMath::Cos(A), FMath::Sin(A), 0.6f).GetSafeNormal();
			Branch(Axis(Z), Dir, Range(500.0f, 700.0f), TrunkRadius(Z) * 0.5f, 0.15f, 750.0f, true);
		}
		Out.Tips.Add({ FVector(Axis(Height)), 800.0f });
		break;
	}
	default:
	{
		// Broadleaf and birch: limbs spiralling up the trunk, each with a side branch.
		const int32 Limbs = Variant == TreeBirch ? 5 : 6;
		for (int32 i = 0; i < Limbs; ++i)
		{
			const float T = FMath::Lerp(0.38f, 0.78f, i / static_cast<float>(Limbs - 1)) + Range(-0.03f, 0.03f);
			const float Z = T * Height;
			const float A = i * 2.4f + Range(-0.4f, 0.4f);
			const FVector3f Dir = FVector3f(FMath::Cos(A), FMath::Sin(A), Range(0.45f, 0.9f)).GetSafeNormal();
			const float Length = Height * Range(0.3f, 0.42f) * (1.15f - T * 0.4f);
			const float LeafSize = Variant == TreeBirch ? 300.0f : 420.0f;
			const TArray<FVector3f> Limb = Branch(Axis(Z), Dir, Length, FMath::Max(TrunkRadius(Z) * 0.5f, 4.0f), 0.25f, LeafSize, false);
			const float SideA = A + (Rand() < 0.5f ? 0.8f : -0.8f);
			const FVector3f SideDir = FVector3f(FMath::Cos(SideA), FMath::Sin(SideA), 0.7f).GetSafeNormal();
			Branch(Limb[2], SideDir, Length * 0.5f, FMath::Max(TrunkRadius(Z) * 0.25f, 2.5f), 0.2f, LeafSize * 0.8f, false);
		}
		Out.Tips.Add({ FVector(Axis(Height * 0.97f)), Variant == TreeBirch ? 320.0f : 460.0f });
		break;
	}
	}

	Out.TrunkRadius = Radius;
	Out.TrunkHeight = TrunkTop;

	UStaticMesh* Mesh = NewObject<UStaticMesh>(this, *FString::Printf(TEXT("SM_Tree%d"), static_cast<int32>(Variant)));
	Mesh->GetStaticMaterials().Add(FStaticMaterial(BarkMaterial, TEXT("Bark"), TEXT("Bark")));
	UStaticMesh::FBuildMeshDescriptionsParams Params;
	Params.bFastBuild = true;
	Params.bBuildSimpleCollision = false;
	Mesh->BuildFromMeshDescriptions({ &Description }, Params);
	return Mesh;
}

void ATerrainStreamer::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Workers read this actor; let them finish before it goes away.
	UE::Tasks::Wait(Tasks);
	Tasks.Reset();
	Super::EndPlay(EndPlayReason);
}

void ATerrainStreamer::LaunchJob(TFunction<void(FJobResult&)>&& Work, EJobKind Kind, const FIntPoint& Coord)
{
	Tasks.Add(UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, Work = MoveTemp(Work), Kind, Coord]()
	{
		TSharedPtr<FJobResult, ESPMode::ThreadSafe> Result = MakeShared<FJobResult, ESPMode::ThreadSafe>();
		Result->Kind = Kind;
		Result->Coord = Coord;
		Work(*Result);
		Results.Enqueue(Result);
	}, UE::Tasks::ETaskPriority::BackgroundHigh));
}

float ATerrainStreamer::RingEnd(int32 Step) const
{
	switch (Step)
	{
	case 1: return 5.5f;
	case 2: return 9.5f;
	case 4: return 13.5f;
	default: return 0.0f; // step 8 meets the far tiles, which are the same resolution
	}
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

	// Estimate where we're heading, so work goes to what's about to come into view.
	if (DeltaTime > 0.0f && !LastViewLocation.IsZero())
	{
		const FVector Instant = (ViewLocation - LastViewLocation) / DeltaTime;
		ViewVelocity = Instant.Size() > 200000.0f ? FVector::ZeroVector : FMath::VInterpTo(ViewVelocity, Instant, DeltaTime, 3.0f);
	}
	LastViewLocation = ViewLocation;
	const float ChunkSize = ChunkWorldSize();
	FVector2D Ahead = FVector2D(ViewVelocity) * LookAheadSeconds / ChunkSize;
	if (Ahead.Size() > 6.0f) { Ahead = Ahead.GetSafeNormal() * 6.0f; }
	const FVector2D ViewChunk = FVector2D(ViewLocation) / ChunkSize;
	const FVector2D Predicted = ViewChunk + Ahead;
	const FVector2D Facing = FVector2D(ViewRotation.Vector()).GetSafeNormal();

	// The ocean is one big plane at sea level that follows the viewer, reaching well past the fog so its edge never shows.
	Ocean->SetWorldLocation(FVector(ViewLocation.X, ViewLocation.Y, 0.0));
	Ocean->SetWorldScale3D(FVector(200000.0, 200000.0, 1.0));

	// Upload finished work within the frame budget.
	const double Deadline = FPlatformTime::Seconds() + BuildBudgetMs * 0.001;
	TSharedPtr<FJobResult, ESPMode::ThreadSafe> Result;
	while (FPlatformTime::Seconds() < Deadline && Results.Dequeue(Result))
	{
		ApplyChunk(*Result, Center);
	}
	Tasks.RemoveAll([](const UE::Tasks::FTask& Task) { return Task.IsCompleted(); });

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

	// Work out what each chunk in view should look like, and queue what doesn't match yet: the path
	// ahead first, then what's in front of the camera, then the rest.
	struct FWork
	{
		float Score;
		FIntPoint Coord;
		int32 Step;
		bool bCollision;
		EProps Props;
		bool bSurface;
		bool bProps;
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
			if (ChunkJobs.Contains(Coord))
			{
				continue;
			}
			const float Dist = FMath::Sqrt(static_cast<float>(DistSq));
			const int32 Step = StepForDistance(Dist);
			const bool bCollision = DistSq <= CollisionRadius * CollisionRadius;
			const EProps Props = DistSq <= 0 ? EProps::Near : DistSq <= 4 ? EProps::Shell : DistSq <= DetailRadius * DetailRadius ? EProps::Full
				: (NumLoadedScanned > 0 && DistSq <= TreeRadius * TreeRadius) ? EProps::Trees : EProps::Landmarks;

			const FChunk* Existing = Chunks.Find(Coord);
			bool bSurface = !Existing || !Existing->Mesh || Existing->Step != Step || (bCollision && !Existing->bHasCollision);
			// Refresh the geomorph of chunks in a ring's blend band once the viewer has moved on.
			const float End = RingEnd(Step);
			if (!bSurface && End > 0.0f && Dist > End - 2.5f && (Existing->BuiltCenter - Center).SizeSquared() >= 1)
			{
				bSurface = true;
			}
			const bool bProps = !Existing || Existing->Props != Props;
			if (!bSurface && !bProps)
			{
				continue;
			}
			const FVector2D ChunkMid = FVector2D(Coord) + FVector2D(0.5f);
			const FVector2D ToChunk = ChunkMid - ViewChunk;
			const float InFront = ToChunk.SizeSquared() > 1.0f ? FVector2D::DotProduct(ToChunk.GetSafeNormal(), Facing) : 1.0f;
			const float Score = (0.35f * Dist + 0.65f * FVector2D::Distance(ChunkMid, Predicted)) * (1.0f + 0.4f * (1.0f - InFront)) - (DistSq <= 2 ? 100.0f : 0.0f);
			Work.Add({ Score, Coord, Step, bCollision, Props, bSurface, bProps });
		}
	}
	Work.Sort([](const FWork& A, const FWork& B) { return A.Score < B.Score; });

	// How far behind we are on what's right around and ahead of the viewer.
	{
		int32 Missing = 0;
		for (const FWork& Item : Work)
		{
			Missing += (Item.Score < 5.0f) ? 1 : 0;
		}
		for (const FIntPoint& Coord : ChunkJobs)
		{
			Missing += (FVector2D::Distance(FVector2D(Coord) + FVector2D(0.5f), Predicted) < 4.0f) ? 1 : 0;
		}
		Backlog = FMath::Clamp(Missing / 20.0f, 0.0f, 1.0f);
	}

	for (const FWork& Item : Work)
	{
		if (Tasks.Num() >= MaxJobsInFlight)
		{
			break;
		}
		ChunkJobs.Add(Item.Coord);
		const FVector Origin(Item.Coord.X * ChunkSize, Item.Coord.Y * ChunkSize, 0.0);
		const float End = RingEnd(Item.Step) * ChunkSize;
		const FVector2D MorphCenter(ViewLocation);
		LaunchJob([this, Item, Origin, End, MorphCenter, Center](FJobResult& Out)
		{
			Out.Step = Item.Step;
			Out.bCollision = Item.bCollision;
			Out.Props = Item.Props;
			Out.BuiltCenter = Center;
			if (Item.bSurface)
			{
				Out.bSurface = true;
				ComputeSurface(Out.Surface, Origin, ChunkResolution / Item.Step, GridSpacing * Item.Step, MorphCenter, End > 0.0f ? End - 2.0f * ChunkWorldSize() : 0.0f, End);
				if (Item.Step == 1)
				{
					ComputePaths(Item.Coord, Out.Paths);
				}
			}
			if (Item.bProps)
			{
				Out.bProps = true;
				ComputeProps(Item.Coord, Item.Props, Out);
			}
		}, EJobKind::Chunk, Item.Coord);
	}

	// Solid props only need physics right around the viewer; creating bodies for every trunk in the
	// whole detail ring is what makes streaming slow.
	for (TPair<FIntPoint, FChunk>& Pair : Chunks)
	{
		const bool bWant = (Pair.Key - Center).SizeSquared() <= CollisionRadius * CollisionRadius;
		if (Pair.Value.bPropCollision != bWant)
		{
			for (int32 Part = 0; Part < Tree0; ++Part)
			{
				if (Part != Bush && Part != IslandBush && Part < Furniture0 && Part != BuildSurf0 + static_cast<int32>(HouseGen::ESurface::Glass) && Pair.Value.Parts[Part])
				{
					Pair.Value.Parts[Part]->SetCollisionEnabled(bWant ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
				}
			}
			Pair.Value.bPropCollision = bWant;
		}
	}

	UpdateGrass(ViewLocation, Deadline);
	UpdateFarTerrain(Center, Deadline);
}

void ATerrainStreamer::ApplyChunk(FJobResult& Result, const FIntPoint& Center)
{
	if (Result.Kind == EJobKind::Far)
	{
		FarJobs.Remove(Result.Coord);
		if (!FarTiles.Contains(Result.Coord))
		{
			UProceduralMeshComponent* Mesh = AcquireMesh();
			const double TileSize = ChunkWorldSize() * FarTileChunks;
			ApplySurface(Mesh, FVector(Result.Coord.X * TileSize, Result.Coord.Y * TileSize, -FarTileDrop), Result.Surface, false, TerrainMaterial);
			FarTiles.Add(Result.Coord, Mesh);
		}
		return;
	}
	if (Result.Kind == EJobKind::Grass)
	{
		GrassJobs.Remove(Result.Coord);
		if (!GrassTiles.Contains(Result.Coord))
		{
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
				Component->SetupAttachment(RootComponent);
				Component->RegisterComponent();
				AllParts.Add(Component);
			}
			Component->SetVisibility(true);
			SetInstances(Component, Result.Transforms[0], Result.CustomData[0]);
			GrassTiles.Add(Result.Coord, Component);
		}
		return;
	}

	ChunkJobs.Remove(Result.Coord);
	if ((Result.Coord - Center).SizeSquared() > FMath::Square(ViewRadius + 1))
	{
		return; // flew past it while it was being built
	}
	FChunk& Chunk = Chunks.FindOrAdd(Result.Coord);
	if (Result.bSurface)
	{
		if (!Chunk.Mesh)
		{
			Chunk.Mesh = AcquireMesh();
		}
		ApplySurface(Chunk.Mesh, FVector(Result.Coord.X * ChunkWorldSize(), Result.Coord.Y * ChunkWorldSize(), 0.0), Result.Surface, Result.bCollision, TerrainMaterial);
		// Paths sit exactly on full-detail terrain, so they exist only there.
		if (Result.Step == 1 && Result.Paths.Vertices.Num() > 0)
		{
			if (!Chunk.PathMesh)
			{
				Chunk.PathMesh = AcquireMesh();
			}
			ApplySurface(Chunk.PathMesh, FVector(Result.Coord.X * ChunkWorldSize(), Result.Coord.Y * ChunkWorldSize(), 0.0), Result.Paths, false, PathMaterial);
			Chunk.PathMesh->SetCastShadow(false);
		}
		else if (Chunk.PathMesh)
		{
			Chunk.PathMesh->ClearAllMeshSections();
			Chunk.PathMesh->SetVisibility(false);
			MeshPool.Add(Chunk.PathMesh);
			Chunk.PathMesh = nullptr;
		}
		Chunk.Step = Result.Step;
		Chunk.bHasCollision = Result.bCollision;
		Chunk.BuiltCenter = Result.BuiltCenter;
	}
	if (Result.bProps)
	{
		for (int32 Part = 0; Part < NumParts; ++Part)
		{
			UInstancedStaticMeshComponent*& Component = Chunk.Parts[Part];
			if (Result.Transforms[Part].Num() == 0)
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
			// Start without physics; Tick enables it if this chunk is close enough.
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			SetInstances(Component, Result.Transforms[Part], Result.CustomData[Part]);
		}
		Chunk.Props = Result.Props;
		Chunk.bPropCollision = false;
	}
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
			if (DX * DX + DY * DY <= TileRadius * TileRadius && !FarTiles.Contains(Tile) && !FarJobs.Contains(Tile) && !Covered(Tile))
			{
				Missing.Emplace(DX * DX + DY * DY, Tile);
			}
		}
	}
	Missing.Sort([](const TPair<int32, FIntPoint>& A, const TPair<int32, FIntPoint>& B) { return A.Key < B.Key; });

	const double TileSize = ChunkWorldSize() * FarTileChunks;
	for (const TPair<int32, FIntPoint>& Item : Missing)
	{
		if (Tasks.Num() >= MaxJobsInFlight + 4)
		{
			break;
		}
		FarJobs.Add(Item.Value);
		const FVector Origin(Item.Value.X * TileSize, Item.Value.Y * TileSize, -FarTileDrop);
		LaunchJob([this, Origin, TileSize](FJobResult& Out)
		{
			ComputeSurface(Out.Surface, Origin, FarTileResolution, TileSize / FarTileResolution);
		}, EJobKind::Far, Item.Value);
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

	constexpr int32 Blades = 22;
	constexpr int32 Segments = 3;
	for (int32 Blade = 0; Blade < Blades; ++Blade)
	{
		auto Rand = [Blade](uint32 Seed) { return WorldGen::HashFloat(Blade, 7, 500 + Seed); };
		const float Angle = Rand(0) * 2.0f * PI;
		const float Spread = Rand(1) * 22.0f;
		const FVector3f Base(FMath::Cos(Rand(2) * 2.0f * PI) * Spread, FMath::Sin(Rand(2) * 2.0f * PI) * Spread, 0.0f);
		const FVector3f Facing(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
		const FVector3f Side(-Facing.Y, Facing.X, 0.0f);
		const float Height = FMath::Lerp(40.0f, 95.0f, Rand(3));
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
			if (DX * DX + DY * DY <= Radius * Radius && !GrassTiles.Contains(Tile) && !GrassJobs.Contains(Tile))
			{
				Missing.Emplace(DX * DX + DY * DY, Tile);
			}
		}
	}
	Missing.Sort([](const TPair<int32, FIntPoint>& A, const TPair<int32, FIntPoint>& B) { return A.Key < B.Key; });

	for (const TPair<int32, FIntPoint>& Item : Missing)
	{
		if (Tasks.Num() >= MaxJobsInFlight + 8)
		{
			break;
		}
		GrassJobs.Add(Item.Value);
		const FIntPoint Tile = Item.Value;
		LaunchJob([this, Tile](FJobResult& Out)
		{
			ComputeGrassTile(Tile, Out.Transforms[0], Out.CustomData[0]);
		}, EJobKind::Grass, Tile);
	}
}

void ATerrainStreamer::ComputeGrassTile(const FIntPoint& Tile, TArray<FTransform>& Transforms, TArray<float>& Data) const
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
	TArray<WorldGen::FPath> Paths;
	WorldGen::PathsNear(Origin, Origin + FVector2D(GrassTileSize), 500.0, Paths);

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
			float PathWidth;
			if (Paths.Num() > 0 && WorldGen::DistanceToPath(Paths, P, PathWidth) < PathWidth * 0.5f + 40.0f)
			{
				continue;
			}

			if (RealPlace::Occupied(P))
			{
				continue; // no grass through a real building or house lot
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
			FLinearColor Tint = FMath::Lerp(FLinearColor(1.0f, 1.0f, 1.0f), FLinearColor(1.4f, 1.2f, 0.8f), S.Dryness * 0.7f) * FMath::Lerp(1.0f, 0.75f, S.Forest);
			Tint = Jitter(Tint, WorldGen::HashFloat(HX, HY, 607), 0.15f);
			Data.Append({ Tint.R, Tint.G, Tint.B });
		}
	}
}

void ATerrainStreamer::SetInstances(UInstancedStaticMeshComponent* Component, const TArray<FTransform>& Transforms, const TArray<float>& CustomData)
{
	// Add all instances and write their custom data in one go, then (for hierarchical components)
	// build the culling tree once; Nanite meshes use plain instancing and skip that entirely.
	Component->ClearInstances();
	if (Transforms.Num() == 0)
	{
		return;
	}
	Component->AddInstances(Transforms, false, true);
	if (Component->PerInstanceSMCustomData.Num() == CustomData.Num())
	{
		FMemory::Memcpy(Component->PerInstanceSMCustomData.GetData(), CustomData.GetData(), CustomData.Num() * sizeof(float));
	}
	if (UHierarchicalInstancedStaticMeshComponent* Hierarchical = Cast<UHierarchicalInstancedStaticMeshComponent>(Component))
	{
		Hierarchical->BuildTreeIfOutdated(true, true);
	}
	Component->MarkRenderStateDirty();
}

float ATerrainStreamer::SurfaceHeight(double X, double Y) const
{
	const double GX = X / GridSpacing;
	const double GY = Y / GridSpacing;
	const double IX = FMath::FloorToDouble(GX);
	const double IY = FMath::FloorToDouble(GY);
	const float FX = GX - IX;
	const float FY = GY - IY;
	const float BL = WorldGen::Height(IX * GridSpacing, IY * GridSpacing);
	const float BR = WorldGen::Height((IX + 1) * GridSpacing, IY * GridSpacing);
	const float TL = WorldGen::Height(IX * GridSpacing, (IY + 1) * GridSpacing);
	const float TR = WorldGen::Height((IX + 1) * GridSpacing, (IY + 1) * GridSpacing);
	// Same triangle split as the terrain mesh (diagonal from bottom-right to top-left).
	return FX + FY <= 1.0f
		? BL + FX * (BR - BL) + FY * (TL - BL)
		: TR + (1.0f - FX) * (TL - TR) + (1.0f - FY) * (BR - TR);
}

void ATerrainStreamer::ComputePaths(const FIntPoint& Coord, FSurfaceData& Out) const
{
	const FVector2D Min(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize());
	const FVector2D Max = Min + FVector2D(ChunkWorldSize());
	TArray<WorldGen::FPath> Paths;
	WorldGen::PathsNear(Min, Max, 1000.0, Paths);

	constexpr float Step = 250.0f;
	for (const WorldGen::FPath& Path : Paths)
	{
		const float Length = FVector2D::Distance(Path.A, Path.B);
		const int32 Count = FMath::CeilToInt(Length / Step);
		int32 Prev = INDEX_NONE;
		for (int32 i = 0; i <= Count; ++i)
		{
			const float T = static_cast<float>(i) / Count;
			const FVector2D P = WorldGen::PathPoint(Path, T);
			// Only the stretch over this chunk (with a little overlap so neighbours join seamlessly).
			const bool bInside = P.X >= Min.X - Step && P.X <= Max.X + Step && P.Y >= Min.Y - Step && P.Y <= Max.Y + Step;
			const float Ground = WorldGen::Height(P.X, P.Y);
			if (!bInside || Ground < 40.0f)
			{
				Prev = INDEX_NONE;
				continue;
			}
			const FVector2D Dir = (WorldGen::PathPoint(Path, FMath::Min(T + 0.5f / Count, 1.0f)) - WorldGen::PathPoint(Path, FMath::Max(T - 0.5f / Count, 0.0f))).GetSafeNormal();
			const FVector2D Side(-Dir.Y, Dir.X);
			const int32 Base = Out.Vertices.Num();
			for (const float Edge : { -0.5f, 0.5f })
			{
				const FVector2D Q = P + Side * Path.Width * Edge;
				Out.Vertices.Add(FVector(Q.X - Min.X, Q.Y - Min.Y, SurfaceHeight(Q.X, Q.Y) + 8.0f));
				Out.Normals.Add(FVector::UpVector);
				Out.UV0.Add(FVector2D(Edge + 0.5f, T * Length / 400.0f));
				Out.Colors.Add(Path.bRoad ? FColor(255, 0, 0, 255) : FColor(0, 0, 0, 255));
			}
			if (Prev != INDEX_NONE)
			{
				Out.Triangles.Append({ Prev, Base, Prev + 1, Prev + 1, Base, Base + 1 });
				Out.Triangles.Append({ Prev, Prev + 1, Base, Prev + 1, Base + 1, Base }); // both faces, winding-proof
			}
			Prev = Base;
		}
	}
}

void ATerrainStreamer::ComputeSurface(FSurfaceData& Out, const FVector& Origin, int32 N, float Spacing, const FVector2D& MorphCenter, float MorphStart, float MorphEnd) const
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

	// Height of the next-coarser grid (twice the spacing) at a fine vertex, using the coarse mesh's own
	// triangle split, so a fully morphed edge matches the neighbouring coarser chunk exactly.
	auto CoarseHeight = [&](int32 X, int32 Y)
	{
		const int32 IX = FMath::Min((X / 2) * 2, N - 2);
		const int32 IY = FMath::Min((Y / 2) * 2, N - 2);
		const float FX = (X - IX) * 0.5f;
		const float FY = (Y - IY) * 0.5f;
		const float BL = At(IX, IY).Height, BR = At(IX + 2, IY).Height, TL = At(IX, IY + 2).Height, TR = At(IX + 2, IY + 2).Height;
		return FX + FY <= 1.0f
			? BL + FX * (BR - BL) + FY * (TL - BL)
			: TR + (1.0f - FX) * (TL - TR) + (1.0f - FY) * (BR - TR);
	};
	const bool bMorph = MorphEnd > MorphStart && N >= 2 && (N % 2) == 0;

	const int32 NumSkirt = 4 * N;
	const int32 NumVerts = Side * Side + NumSkirt;
	Out.Vertices.Reserve(NumVerts);
	Out.Normals.Reserve(NumVerts);
	Out.UV0.Reserve(NumVerts);
	Out.UV1.Reserve(NumVerts);
	Out.Colors.Reserve(NumVerts);
	Out.Triangles.Reserve(N * N * 6 + NumSkirt * 12);

	auto ToByte = [](float V) { return static_cast<uint8>(FMath::Clamp(V, 0.0f, 1.0f) * 255.0f); };
	for (int32 Y = 0; Y < Side; ++Y)
	{
		for (int32 X = 0; X < Side; ++X)
		{
			const FWorldSample& S = At(X, Y);
			float Height = S.Height;
			if (bMorph)
			{
				const float Dist = FVector2D::Distance(MorphCenter, FVector2D(Origin.X + X * Spacing, Origin.Y + Y * Spacing));
				const float Morph = FMath::Clamp((Dist - MorphStart) / (MorphEnd - MorphStart), 0.0f, 1.0f);
				Height = FMath::Lerp(Height, CoarseHeight(X, Y), Morph);
			}
			Out.Vertices.Add(FVector(X * Spacing, Y * Spacing, Height));

			// Central-difference normal from neighbouring heights.
			const float DX = At(X + 1, Y).Height - At(X - 1, Y).Height;
			const float DY = At(X, Y + 1).Height - At(X, Y - 1).Height;
			const FVector Normal = FVector(-DX, -DY, 2.0f * Spacing).GetSafeNormal();
			Out.Normals.Add(Normal);

			// Material layer weights (see M_Terrain): R sand, G rock, B snow, A forest floor.
			const float Snow = S.Snow * Smooth(0.5f, 0.72f, Normal.Z);
			const float Steep = Smooth(0.82f, 0.6f, Normal.Z) * Smooth(200.0f, 800.0f, S.Height);
			const float Rock = FMath::Max(Steep, S.Rock) * (1.0f - Snow * 0.8f);
			Out.Colors.Add(FColor(ToByte(S.Sand * (1.0f - Rock)), ToByte(Rock), ToByte(Snow), ToByte(S.Forest)));
			Out.UV1.Add(FVector2D(S.Dryness, S.Wetness));
			Out.UV0.Add(FVector2D(X, Y));
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
			Out.Triangles.Append({ BottomLeft, TopLeft, BottomRight, BottomRight, TopLeft, TopRight });
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
	const int32 SkirtStart = Out.Vertices.Num();
	for (int32 i = 0; i < NumSkirt; ++i)
	{
		// Copy before adding: Add() may reallocate the array the reference points into.
		const int32 Top = Perimeter[i];
		const FVector Vertex = Out.Vertices[Top] - FVector(0, 0, SkirtDepth);
		const FVector Normal = Out.Normals[Top];
		const FColor Layer = Out.Colors[Top];
		const FVector2D A = Out.UV0[Top];
		const FVector2D B = Out.UV1[Top];
		Out.Vertices.Add(Vertex);
		Out.Normals.Add(Normal);
		Out.Colors.Add(Layer);
		Out.UV0.Add(A);
		Out.UV1.Add(B);
	}
	for (int32 i = 0; i < NumSkirt; ++i)
	{
		const int32 A = Perimeter[i];
		const int32 B = Perimeter[i + 1];
		const int32 A2 = SkirtStart + i;
		const int32 B2 = SkirtStart + (i + 1) % NumSkirt;
		// Both windings, so the curtain is visible from either side.
		Out.Triangles.Append({ A, A2, B, B, A2, B2, A, B, A2, B, B2, A2 });
	}
}

void ATerrainStreamer::ApplySurface(UProceduralMeshComponent* Mesh, const FVector& Origin, FSurfaceData& Data, bool bWithCollision, UMaterialInterface* Material) const
{
	Mesh->SetWorldLocation(Origin);
	const TArray<FVector2D> Empty;
	Mesh->CreateMeshSection(0, Data.Vertices, Data.Triangles, Data.Normals, Data.UV0, Data.UV1, Empty, Empty, Data.Colors, TArray<FProcMeshTangent>(), bWithCollision);
	Mesh->SetMaterial(0, Material);
	Mesh->SetVisibility(true);
}

void ATerrainStreamer::FPropBatch::Add(EPropPart Part, const FVector& Center, const FRotator& Rotation, const FVector& SizeCm, const FLinearColor& Color, float Glow, ESurface Surface)
{
	Instances[Part].Add({ Center, Rotation, SizeCm });
	CustomData[Part].Append({ Color.R, Color.G, Color.B, Glow, static_cast<float>(Surface) });
}

void ATerrainStreamer::ComputeProps(const FIntPoint& Coord, EProps Level, FJobResult& Out) const
{
	FPropBatch Batch;
	// Council Bluffs is a real place: no generated cities, islands or landmarks over it.
	const FVector2D ChunkMin2(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize());
	if (!RealPlace::Covers(ChunkMin2, ChunkMin2 + FVector2D(ChunkWorldSize()), 250000.0))
	{
		AddCities(Coord, Batch);
		AddLighthouses(Coord, Batch);
		AddVolcanoGlow(Coord, Batch);
		AddFloatingIslands(Coord, Batch);
		AddDiscoveries(Coord, Batch);
	}
	AddRealBuildings(Coord, Batch, Level);
	if (Level == EProps::Trees)
	{
		AddVegetation(Coord, Batch, true);
	}
	if (Level >= EProps::Full)
	{
		AddVegetation(Coord, Batch, false);
		if (!RealPlace::Covers(ChunkMin2, ChunkMin2 + FVector2D(ChunkWorldSize()), 250000.0))
		{
			AddVillages(Coord, Batch);
			AddStoneCircles(Coord, Batch);
		}
	}

	for (int32 Part = 0; Part < NumParts; ++Part)
	{
		const TArray<FPropBatch::FInstance>& Instances = Batch.Instances[Part];
		if (Instances.Num() == 0 || !PartBounds.IsValidIndex(Part))
		{
			continue;
		}
		// Fit each mesh to the requested size and centre it, whatever its native size and pivot.
		const FBox& Bounds = PartBounds[Part];
		const FVector MeshSize = Bounds.GetSize().ComponentMax(FVector(1.0));
		TArray<FTransform>& Transforms = Out.Transforms[Part];
		Transforms.Reserve(Instances.Num());
		for (const FPropBatch::FInstance& Instance : Instances)
		{
			// Trees are placed at their base with a uniform scale in Size.
			if (Part >= Tree0)
			{
				Transforms.Add(FTransform(Instance.Rotation, Instance.Center, Instance.Size));
				continue;
			}
			// Furniture keeps its real size; Center is where the middle of its base sits on the floor.
			if (Part >= Furniture0)
			{
				const FVector Pivot(Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.Min.Z);
				Transforms.Add(FTransform(Instance.Rotation, Instance.Center - Instance.Rotation.RotateVector(Pivot * Instance.Size), Instance.Size));
				continue;
			}
			const FVector Scale = Instance.Size / MeshSize;
			const FVector Location = Instance.Center - Instance.Rotation.RotateVector(Bounds.GetCenter() * Scale);
			Transforms.Add(FTransform(Instance.Rotation, Location, Scale));
		}
		Out.CustomData[Part] = MoveTemp(Batch.CustomData[Part]);
	}
}

void ATerrainStreamer::AddVegetation(const FIntPoint& Coord, FPropBatch& Batch, bool bTreesOnly) const
{
	constexpr double Cell = 1300.0;
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
	TArray<WorldGen::FPath> Paths;
	WorldGen::PathsNear(FVector2D(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize()), FVector2D((Coord.X + 1) * ChunkWorldSize(), (Coord.Y + 1) * ChunkWorldSize()), 1000.0, Paths);

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
			float PathWidth;
			if (bCleared || (Paths.Num() > 0 && WorldGen::DistanceToPath(Paths, FVector2D(PX, PY), PathWidth) < PathWidth * 0.5f + 300.0f))
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

			// A tree: the branching wood mesh, an invisible collider round the trunk, and scanned foliage
			// clusters on each branch tip so the leaves grow from the branches.
			auto PlaceTree = [&](int32 Variant, float Scale, const FLinearColor& BarkTint, const FLinearColor& Leaves, float LeafScale, float Flatten)
			{
				const FTreeTemplate& T = TreeTemplates[Variant];
				Batch.Add(static_cast<EPropPart>(Tree0 + Variant), Ground, Spin, FVector(Scale), BarkTint);
				Batch.Add(Trunk, Ground + FVector(0, 0, T.TrunkHeight * 0.5f * Scale), Spin, FVector(T.TrunkRadius * 2.0f * Scale, T.TrunkRadius * 2.0f * Scale, T.TrunkHeight * Scale), White);
				for (int32 i = 0; i < T.Tips.Num(); ++i)
				{
					const FVector Tip = Ground + Spin.RotateVector(T.Tips[i].Position * Scale);
					const float W = T.Tips[i].Size * Scale * LeafScale;
					const float R = WorldGen::HashFloat(CX * 31 + i, CY, 700);
					Batch.Add(Bush, Tip, FRotator(0.0f, R * 360.0f, 0.0f), FVector(W, W, W * Flatten), Jitter(Leaves, R));
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

				// Temperate broadleaf woods use the photoscanned trees when they're installed.
				const bool bTemperate = S.Biome != EBiome::Desert && S.Biome != EBiome::Savanna && S.Biome != EBiome::Jungle
					&& S.Biome != EBiome::Taiga && S.Biome != EBiome::Tundra && S.Biome != EBiome::Snow;
				if (bTemperate && NumLoadedScanned > 0 && Kind >= 0.24f)
				{
					// Mostly mature canopy trees; a fifth young understory trees (only near, where you'd see them).
					const bool bYoung = YoungSlots.Num() > 0 && Kind < 0.24f + 0.76f * 0.2f;
					if (bYoung && bTreesOnly)
					{
						continue;
					}
					const TArray<int32>& Pool = bYoung ? YoungSlots : MatureSlots;
					const int32 Slot = Pool[FMath::Min(FMath::FloorToInt(Tint * Pool.Num()), Pool.Num() - 1)];
					// Scanned at real size; vary a little, and sink the root flare into the ground.
					const float Scale = TreeScale * (bYoung ? FMath::Lerp(0.75f, 1.0f, WorldGen::HashFloat(CX, CY, SeedTreeSize))
						: FMath::Lerp(0.85f, 1.1f, WorldGen::HashFloat(CX, CY, SeedTreeSize)));
					Batch.Add(static_cast<EPropPart>(Scanned0 + Slot), Ground - FVector(0, 0, 25.0f), Spin, FVector(Scale), White);
					if (!bTreesOnly)
					{
						const float H = ScannedTreeHeight[Slot] * Scale;
						Batch.Add(Trunk, Ground + FVector(0, 0, H * 0.2f), Spin, FVector(80.0f * Scale, 80.0f * Scale, H * 0.4f), White);
					}
					continue;
				}
				if (bTreesOnly)
				{
					continue;
				}

				switch (S.Biome)
				{
				case EBiome::Desert:
				case EBiome::Savanna:
					PlaceTree(TreeAcacia, Size, BarkWarm, LeafDry, 1.0f, 0.45f);
					break;
				case EBiome::Jungle:
					PlaceTree(TreeJungle, Size, BarkPale, LeafJungle, 1.0f, 0.7f);
					break;
				case EBiome::Taiga:
				case EBiome::Tundra:
				case EBiome::Snow:
				{
					// Conifers, frosted in the deep cold.
					const FLinearColor Needles = FMath::Lerp(LeafConifer, LeafFrost, Smooth(0.22f, 0.08f, S.Temperature));
					PlaceTree(Kind < 0.5f ? TreeConiferA : TreeConiferB, Size, BarkDark, Needles, 1.0f, 0.5f);
					break;
				}
				default:
				{
					// Broadleaf of several shapes, some birch, the odd conifer, a few already turning gold or rust.
					if (Kind < 0.14f)
					{
						PlaceTree(Kind < 0.07f ? TreeConiferA : TreeConiferB, Size, BarkDark, LeafConifer, 1.0f, 0.5f);
						break;
					}
					FLinearColor Leaves = LeafTemperate;
					if (Kind > 0.93f) Leaves = LeafGold;
					else if (Kind > 0.88f) Leaves = LeafRust;
					if (Kind < 0.24f)
					{
						PlaceTree(TreeBirch, Size, BarkBirch, Leaves, 0.9f, 0.9f);
						break;
					}
					PlaceTree(TreeBroadleafA + FMath::FloorToInt(Tint * 2.999f), Size, BarkWarm, Leaves, 1.0f, 0.85f);
					break;
				}
				}
			}
			else if (bTreesOnly)
			{
				continue;
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
	// Villages grow at road junctions (the village grid is the road grid).
	static_assert(VillageCell == 250000.0, "villages sit on WorldGen road nodes");
	OutCenter = FVector(WorldGen::RoadNode(CX, CY), 0.0);
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

			// Lay out every house in the village first (the same way whichever chunk asks), skipping any that
			// would overlap an earlier one, then build the ones standing in this chunk.
			const int32 Houses = 6 + FMath::FloorToInt(Rand.Next() * 14.0f);
			TArray<FVector4> Placed; // x, y, radius, yaw
			for (int32 i = 0; i < Houses; ++i)
			{
				FRandom House{ CX * 131 + i, CY, SeedVillage + 1 };
				const float Angle = House.Range(0.0f, 2.0f * PI);
				const float Radius = i == 0 ? 0.0f : HouseScale * (2500.0f + 17000.0f * FMath::Sqrt(House.Next()));
				const FVector Pos = Center + FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, 0.0);
				const float Yaw = House.Range(0.0f, 360.0f);
				const float Footprint = HouseScale * (i == 0 ? 1500.0f : 850.0f);
				bool bOverlaps = false;
				for (const FVector4& P : Placed)
				{
					bOverlaps |= FVector2D::Distance(FVector2D(P.X, P.Y), FVector2D(Pos)) < P.Z + Footprint + 200.0f;
				}
				if (bOverlaps)
				{
					continue;
				}
				Placed.Add(FVector4(Pos.X, Pos.Y, Footprint, Yaw));
				if (!ChunkContains(Coord, Pos))
				{
					continue;
				}
				const float Ground = WorldGen::Height(Pos.X, Pos.Y);
				if (Ground < 150.0f)
				{
					continue;
				}

				if (i == 0)
				{
					// A church at the heart of the village, with an interior (see HouseGen::GenerateChurch).
					const HouseGen::FHouse Church = HouseGen::GenerateChurch(WorldGen::Hash(CX * 131, CY, SeedVillage + 4), bAdobe ? HouseGen::EStyle::Plaster : HouseGen::EStyle::Brick);
					const FQuat ChurchQuat = FRotator(0.0f, Yaw, 0.0f).Quaternion();
					float ChurchHigh = Ground;
					for (const FVector2D Corner : { FVector2D(-1, -1), FVector2D(1, -1), FVector2D(-1, 1), FVector2D(1, 1) })
					{
						const FVector C = FVector(Pos.X, Pos.Y, 0.0) + ChurchQuat.RotateVector(FVector(Corner.X * Church.Width * 0.5f, Corner.Y * Church.Depth * 0.5f, 0.0f) * HouseScale);
						ChurchHigh = FMath::Max(ChurchHigh, WorldGen::Height(C.X, C.Y));
					}
					const FVector ChurchBase(Pos.X, Pos.Y, ChurchHigh + 40.0f * HouseScale);
					for (const HouseGen::FPiece& Piece : Church.Pieces)
					{
						const FVector World = ChurchBase + ChurchQuat.RotateVector(Piece.Center * HouseScale);
						const FRotator PieceRot = (ChurchQuat * Piece.Rotation.Quaternion()).Rotator();
						FVector Size = Piece.Size * HouseScale;
						FVector Mid = World;
						if (Piece.Surface == HouseGen::ESurface::Stone && Piece.Center.Z < 0.0f)
						{
							const float Extra = ChurchHigh - Ground + 150.0f;
							Size.Z += Extra;
							Mid.Z -= Extra * 0.5f;
						}
						Batch.Add(static_cast<EPropPart>(BuildSurf0 + static_cast<int32>(Piece.Surface)), Mid, PieceRot, Size, White);
					}
					continue;
				}

				// A real house: rooms, doors, windows, stairs and furniture (see HouseGen).
				const HouseGen::EStyle Style = bTimber ? HouseGen::EStyle::Timber
					: (!bAdobe && House.Next() < 0.4f) ? HouseGen::EStyle::Brick : HouseGen::EStyle::Plaster;
				const bool bTwoStorey = !bAdobe && House.Next() < 0.5f;
				const HouseGen::FHouse Plan = HouseGen::Generate(WorldGen::Hash(CX * 131 + i, CY, SeedVillage + 3), Style, bTwoStorey);
				// Sit the ground floor just above the highest corner of the footprint; the plinth goes down to the lowest.
				const FRotator Rot(0.0f, Yaw, 0.0f);
				float High = Ground;
				for (const FVector2D Corner : { FVector2D(-1, -1), FVector2D(1, -1), FVector2D(-1, 1), FVector2D(1, 1) })
				{
					const FVector C = FVector(Pos.X, Pos.Y, 0.0) + Rot.RotateVector(FVector(Corner.X * Plan.Width * 0.5f, Corner.Y * Plan.Depth * 0.5f, 0.0f) * HouseScale);
					High = FMath::Max(High, WorldGen::Height(C.X, C.Y));
				}
				const FVector Base(Pos.X, Pos.Y, High + 40.0f * HouseScale);
				static const bool bReportHouses = FParse::Param(FCommandLine::Get(), TEXT("HouseReport"));
				if (bReportHouses)
				{
					UE_LOG(LogTemp, Display, TEXT("HouseReport: x=%.0f y=%.0f z=%.0f yaw=%.0f w=%.0f d=%.0f floors=%d"), Base.X, Base.Y, Base.Z, Yaw, Plan.Width, Plan.Depth, bTwoStorey ? 2 : 1);
				}
				const FLinearColor Wash = bAdobe ? FLinearColor(1.15f, 0.95f, 0.75f) : Jitter(Plaster[FMath::FloorToInt(House.Next() * 3) % 3] * 0.65f, House.Next(), 0.05f);
				const FQuat HouseQuat = Rot.Quaternion();
				for (const HouseGen::FPiece& Piece : Plan.Pieces)
				{
					const FVector World = Base + HouseQuat.RotateVector(Piece.Center * HouseScale);
					const FRotator PieceRot = (HouseQuat * Piece.Rotation.Quaternion()).Rotator();
					if (Piece.bFurniture)
					{
						Batch.Add(static_cast<EPropPart>(Furniture0 + static_cast<int32>(Piece.Furniture)), World, PieceRot, FVector(HouseScale), White);
						continue;
					}
					FVector Size = Piece.Size * HouseScale;
					FVector Mid = World;
					// Extend the plinth down to the terrain on sloping ground.
					if (Piece.Surface == HouseGen::ESurface::Stone && Piece.Center.Z < 0.0f)
					{
						const float Extra = High - Ground + 150.0f;
						Size.Z += Extra;
						Mid.Z -= Extra * 0.5f;
					}
					const FLinearColor Tint = Piece.Surface == HouseGen::ESurface::ExteriorWall ? Wash : White;
					Batch.Add(static_cast<EPropPart>(BuildSurf0 + static_cast<int32>(Piece.Surface)), Mid, PieceRot, Size, Tint);
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

double ATerrainStreamer::RuinCellSize() { return RuinCell; }
double ATerrainStreamer::ObeliskCellSize() { return ObeliskCell; }
double ATerrainStreamer::WorldTreeCellSize() { return WorldTreeCell; }
double ATerrainStreamer::SkyGateCellSize() { return SkyGateCell; }
double ATerrainStreamer::WindFarmCellSize() { return WindFarmCell; }

bool ATerrainStreamer::FindRuin(int32 CX, int32 CY, FVector& OutCenter)
{
	FRandom Rand{ CX, CY, SeedRuin };
	if (Rand.Next() > 0.3f)
	{
		return false;
	}
	OutCenter = FVector((CX + Rand.Range(0.2f, 0.8f)) * RuinCell, (CY + Rand.Range(0.2f, 0.8f)) * RuinCell, 0.0);
	const FWorldSample S = WorldGen::Sample(OutCenter.X, OutCenter.Y);
	OutCenter.Z = S.Height;
	return S.Height >= 400.0f && S.Height <= 25000.0f && S.Mountains <= 0.4f && S.Volcano <= 0.0f && S.Biome != EBiome::Beach && S.Biome != EBiome::Snow;
}

bool ATerrainStreamer::FindObelisks(int32 CX, int32 CY, FVector& OutCenter)
{
	FRandom Rand{ CX, CY, SeedObelisk };
	if (Rand.Next() > 0.28f)
	{
		return false;
	}
	OutCenter = FVector((CX + Rand.Range(0.2f, 0.8f)) * ObeliskCell, (CY + Rand.Range(0.2f, 0.8f)) * ObeliskCell, 0.0);
	const FWorldSample S = WorldGen::Sample(OutCenter.X, OutCenter.Y);
	OutCenter.Z = S.Height;
	return S.Height >= 400.0f && S.Mountains <= 0.5f && S.Volcano <= 0.0f && S.Biome != EBiome::Beach;
}

bool ATerrainStreamer::FindWorldTree(int32 CX, int32 CY, FVector& OutBase)
{
	FRandom Rand{ CX, CY, SeedWorldTree };
	if (Rand.Next() > 0.4f)
	{
		return false;
	}
	OutBase = FVector((CX + Rand.Range(0.2f, 0.8f)) * WorldTreeCell, (CY + Rand.Range(0.2f, 0.8f)) * WorldTreeCell, 0.0);
	const FWorldSample S = WorldGen::Sample(OutBase.X, OutBase.Y);
	OutBase.Z = S.Height;
	return S.Height >= 400.0f && S.Height <= 12000.0f && S.Moisture > 0.4f && S.Mountains <= 0.2f && S.Volcano <= 0.0f && S.Biome != EBiome::Desert && S.Biome != EBiome::Snow && S.Biome != EBiome::Beach;
}

bool ATerrainStreamer::FindSkyGate(int32 CX, int32 CY, FVector& OutCenter)
{
	FRandom Rand{ CX, CY, SeedSkyGate };
	if (Rand.Next() > 0.35f)
	{
		return false;
	}
	OutCenter = FVector((CX + Rand.Range(0.15f, 0.85f)) * SkyGateCell, (CY + Rand.Range(0.15f, 0.85f)) * SkyGateCell, 0.0);
	// A gate hangs 250-450 m up, over land or sea.
	OutCenter.Z = FMath::Max(WorldGen::Height(OutCenter.X, OutCenter.Y), 0.0f) + Rand.Range(25000.0f, 45000.0f);
	return true;
}

bool ATerrainStreamer::FindWindFarm(int32 CX, int32 CY, FVector& OutCenter)
{
	FRandom Rand{ CX, CY, SeedWindFarm };
	if (Rand.Next() > 0.35f)
	{
		return false;
	}
	OutCenter = FVector((CX + Rand.Range(0.25f, 0.75f)) * WindFarmCell, (CY + Rand.Range(0.25f, 0.75f)) * WindFarmCell, 0.0);
	const FWorldSample S = WorldGen::Sample(OutCenter.X, OutCenter.Y);
	OutCenter.Z = S.Height;
	return S.Height >= 500.0f && S.Height <= 8000.0f && S.Mountains <= 0.15f && S.Volcano <= 0.0f && S.TreeDensity < 0.35f && S.Biome != EBiome::Snow && S.Biome != EBiome::Beach;
}

void ATerrainStreamer::AddDiscoveries(const FIntPoint& Coord, FPropBatch& Batch) const
{
	const FVector2D Min(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize());
	const FVector2D Max = Min + FVector2D(ChunkWorldSize());
	auto ForCells = [&](double Cell, double Radius, TFunctionRef<void(int32, int32)> Fn)
	{
		for (int32 CY = FMath::FloorToInt((Min.Y - Radius) / Cell); CY <= FMath::FloorToInt((Max.Y + Radius) / Cell); ++CY)
		{
			for (int32 CX = FMath::FloorToInt((Min.X - Radius) / Cell); CX <= FMath::FloorToInt((Max.X + Radius) / Cell); ++CX)
			{
				Fn(CX, CY);
			}
		}
	};
	auto Owns = [&](const FVector& P) { return P.X >= Min.X && P.X < Max.X && P.Y >= Min.Y && P.Y < Max.Y; };
	const FLinearColor Sandstone(1.55f, 1.42f, 1.22f);
	const FLinearColor Basalt(0.22f, 0.22f, 0.25f);
	const FLinearColor Cyan(0.25f, 0.75f, 1.0f);

	// Ancient ruins: a platform, a colonnade of which some columns stand, some are broken and some
	// have fallen, an arch, and an altar.
	ForCells(RuinCell, RuinRadius, [&](int32 CX, int32 CY)
	{
		FVector C;
		if (!FindRuin(CX, CY, C))
		{
			return;
		}
		FRandom Rand{ CX, CY, SeedRuin + 1 };
		const float Yaw = Rand.Range(0.0f, 360.0f);
		const FRotator Rot(0, Yaw, 0);
		float High = C.Z;
		for (const FVector2D K : { FVector2D(-1, -1), FVector2D(1, -1), FVector2D(-1, 1), FVector2D(1, 1) })
		{
			const FVector P = C + Rot.RotateVector(FVector(K.X * 3200.0f, K.Y * 2200.0f, 0.0f));
			High = FMath::Max(High, WorldGen::Height(P.X, P.Y));
		}
		const float Floor = High + 60.0f;
		auto Local = [&](float X, float Y, float Z) { return FVector(C.X, C.Y, 0.0) + Rot.RotateVector(FVector(X, Y, 0.0f)) + FVector(0, 0, Z); };
		auto Place = [&](EPropPart Part, const FVector& P, const FRotator& R, const FVector& Size, const FLinearColor& Color, float Glow = 0.0f)
		{
			if (Owns(P))
			{
				Batch.Add(Part, P, R, Size, Color, Glow, SurfConcrete);
			}
		};
		// Stepped platform, sunk into the slope.
		Place(Cube, Local(0, 0, (High - 500.0f + Floor) * 0.5f), Rot, FVector(6800, 4800, Floor - High + 500.0f), Sandstone * 0.85f);
		Place(Cube, Local(0, 0, Floor + 40.0f), Rot, FVector(6200, 4200, 80), Sandstone);
		const float Top = Floor + 80.0f;
		// Two rows of columns.
		for (int32 Row = 0; Row < 2; ++Row)
		{
			for (int32 i = 0; i < 9; ++i)
			{
				const float X = -2800.0f + i * 700.0f;
				const float Y = Row == 0 ? -1500.0f : 1500.0f;
				const float Roll = Rand.Next();
				const FVector Base = Local(X, Y, Top);
				if (Roll < 0.55f)
				{
					// Standing, with a capital; neighbours in a row sometimes share a lintel.
					Place(Cylinder, Base + FVector(0, 0, 550.0f), FRotator::ZeroRotator, FVector(110, 110, 1100), Sandstone);
					Place(Cube, Base + FVector(0, 0, 1130.0f), Rot, FVector(160, 160, 60), Sandstone * 0.95f);
					if (i + 1 < 9 && Rand.Next() < 0.6f)
					{
						Place(Cube, Local(X + 350.0f, Y, Top + 1190.0f), Rot, FVector(760, 150, 100), Sandstone * 0.9f);
					}
				}
				else if (Roll < 0.8f)
				{
					// Broken off partway.
					const float H = Rand.Range(150.0f, 700.0f);
					Place(Cylinder, Base + FVector(0, 0, H * 0.5f), FRotator::ZeroRotator, FVector(110, 110, H), Sandstone * 0.9f);
				}
				else
				{
					// Fallen and lying across the floor.
					const float L = Rand.Range(500.0f, 1000.0f);
					const float FallYaw = Yaw + Rand.Range(-40.0f, 40.0f) + 90.0f;
					Place(Cylinder, Base + FVector(0, 0, 55.0f), FRotator(90, FallYaw, 0), FVector(110, 110, L), Sandstone * 0.9f);
				}
			}
		}
		// A tall arch and an altar between the rows.
		Place(Cylinder, Local(-3400.0f, 0, Top + 700.0f), FRotator::ZeroRotator, FVector(160, 160, 1400), Sandstone);
		Place(Cylinder, Local(-3400.0f, 700.0f, Top + 700.0f), FRotator::ZeroRotator, FVector(160, 160, 1400), Sandstone);
		Place(Cube, Local(-3400.0f, 350.0f, Top + 1450.0f), Rot, FVector(240, 1000, 160), Sandstone);
		Place(Cube, Local(0, 0, Top + 60.0f), Rot, FVector(500, 260, 120), Sandstone * 0.85f);
	});

	// Obelisks: a ring of tall dark monoliths with a glowing seam, around a floating light.
	ForCells(ObeliskCell, ObeliskRadius, [&](int32 CX, int32 CY)
	{
		FVector C;
		if (!FindObelisks(CX, CY, C))
		{
			return;
		}
		FRandom Rand{ CX, CY, SeedObelisk + 1 };
		const int32 Count = 7 + FMath::FloorToInt(Rand.Next() * 4.0f);
		const float Ring = Rand.Range(2800.0f, 3600.0f);
		for (int32 i = 0; i < Count; ++i)
		{
			const float A = 2.0f * PI * i / Count;
			const FVector2D P(C.X + FMath::Cos(A) * Ring, C.Y + FMath::Sin(A) * Ring);
			const FVector Ground(P.X, P.Y, WorldGen::Height(P.X, P.Y));
			const float H = Rand.Range(2200.0f, 4200.0f);
			const FRotator R(Rand.Range(-2.0f, 2.0f), A * 57.2958f + 90.0f, Rand.Range(-2.0f, 2.0f));
			if (Owns(Ground))
			{
				Batch.Add(Cube, Ground + FVector(0, 0, H * 0.5f - 200.0f), R, FVector(240, 240, H), Basalt, 0.0f, SurfRock);
				// Glowing seam down the face that looks at the centre.
				const FVector Inward = R.RotateVector(FVector(0, 1, 0));
				Batch.Add(Cube, Ground + FVector(0, 0, H * 0.5f - 200.0f) - Inward * 121.0f, R, FVector(40, 30, H * 0.7f), Cyan, 6.0f, SurfConcrete);
			}
		}
		const FVector Orb(C.X, C.Y, WorldGen::Height(C.X, C.Y) + 1800.0f);
		if (Owns(Orb))
		{
			Batch.Add(Sphere, Orb, FRotator::ZeroRotator, FVector(420, 420, 420), Cyan, 14.0f, SurfConcrete);
			Batch.Add(Sphere, Orb, FRotator::ZeroRotator, FVector(620, 620, 620), Cyan * 0.3f, 3.0f, SurfConcrete);
		}
	});

	// The World Tree: a scanned tree grown to the height of a skyscraper.
	ForCells(WorldTreeCell, WorldTreeRadius, [&](int32 CX, int32 CY)
	{
		FVector C;
		if (!FindWorldTree(CX, CY, C) || !Owns(C))
		{
			return;
		}
		FRandom Rand{ CX, CY, SeedWorldTree + 1 };
		const FRotator Spin(0, Rand.Range(0.0f, 360.0f), 0);
		if (NumLoadedScanned > 0 && MatureSlots.Num() > 0)
		{
			const int32 Slot = MatureSlots[FMath::FloorToInt(Rand.Next() * MatureSlots.Num()) % MatureSlots.Num()];
			Batch.Add(static_cast<EPropPart>(Scanned0 + Slot), C - FVector(0, 0, 100.0f), Spin, FVector(4.5f), White);
			Batch.Add(Trunk, C + FVector(0, 0, 900.0f), Spin, FVector(360, 360, 1800), White);
		}
		else
		{
			const FTreeTemplate& T = TreeTemplates[TreeJungle];
			const float Scale = 7.0f;
			Batch.Add(static_cast<EPropPart>(Tree0 + TreeJungle), C, Spin, FVector(Scale), BarkPale);
			for (const FTreeTemplate::FTip& Tip : T.Tips)
			{
				const float W = Tip.Size * Scale;
				Batch.Add(Bush, C + Spin.RotateVector(Tip.Position * Scale), FRotator(0, Rand.Range(0.0f, 360.0f), 0), FVector(W, W, W * 0.7f), Jitter(LeafJungle, Rand.Next()));
			}
			Batch.Add(Trunk, C + FVector(0, 0, 900.0f), Spin, FVector(800, 800, 1800), White);
		}
	});

	// Sky gates: colossal stone rings hanging in the air, glowing on the inside, to fly through.
	ForCells(SkyGateCell, SkyGateRadius, [&](int32 CX, int32 CY)
	{
		FVector C;
		if (!FindSkyGate(CX, CY, C))
		{
			return;
		}
		FRandom Rand{ CX, CY, SeedSkyGate + 1 };
		const float Yaw = Rand.Range(0.0f, 360.0f);
		const FRotator Rot(0, Yaw, 0);
		const float R = Rand.Range(9000.0f, 13000.0f);
		const int32 Blocks = 40;
		const float Arc = 2.0f * PI * R / Blocks;
		for (int32 i = 0; i < Blocks; ++i)
		{
			const float A = 2.0f * PI * i / Blocks;
			const FVector P = C + Rot.RotateVector(FVector(0, FMath::Cos(A) * R, FMath::Sin(A) * R));
			if (!Owns(P))
			{
				continue;
			}
			const FRotator BlockRot(0.0f, Yaw, A * 57.2958f);
			Batch.Add(Cube, P, BlockRot, FVector(1800, 1300, Arc + 60.0f), Basalt * 1.6f, 0.0f, SurfRock);
			const FVector Inner = C + Rot.RotateVector(FVector(0, FMath::Cos(A) * (R - 660.0f), FMath::Sin(A) * (R - 660.0f)));
			Batch.Add(Cube, Inner, BlockRot, FVector(1500, 40, Arc * 0.85f), Cyan, 5.0f, SurfConcrete);
		}
		// A few stones drifting near the ring.
		for (int32 i = 0; i < 6; ++i)
		{
			const FVector P = C + Rot.RotateVector(FVector(Rand.Range(-3000.0f, 3000.0f), Rand.Range(-1.4f, 1.4f) * R, Rand.Range(-1.4f, 1.4f) * R));
			if (Owns(P))
			{
				const float S = Rand.Range(300.0f, 900.0f);
				Batch.Add(Rock, P, FRotator(Rand.Range(0, 360), Rand.Range(0, 360), Rand.Range(0, 360)), FVector(S), White);
			}
		}
	});

	// Wind farms: ranks of turbines on open ground.
	ForCells(WindFarmCell, WindFarmRadius, [&](int32 CX, int32 CY)
	{
		FVector C;
		if (!FindWindFarm(CX, CY, C))
		{
			return;
		}
		FRandom Rand{ CX, CY, SeedWindFarm + 1 };
		const float Yaw = Rand.Range(0.0f, 360.0f);
		const FRotator Rot(0, Yaw, 0);
		const float WindYaw = Yaw + 90.0f + Rand.Range(-25.0f, 25.0f);
		const FRotator Facing(0, WindYaw, 0);
		const FLinearColor Paint(1.9f, 1.9f, 1.9f);
		for (int32 Row = -1; Row <= 1; ++Row)
		{
			for (int32 Col = -3; Col <= 3; ++Col)
			{
				const FVector Off = Rot.RotateVector(FVector(Col * 5500.0f + Rand.Range(-600.0f, 600.0f), Row * 6500.0f + Rand.Range(-800.0f, 800.0f), 0.0f));
				const FVector2D P(C.X + Off.X, C.Y + Off.Y);
				if (!(P.X >= Min.X && P.X < Max.X && P.Y >= Min.Y && P.Y < Max.Y))
				{
					continue;
				}
				const float Ground = WorldGen::Height(P.X, P.Y);
				if (Ground < 300.0f)
				{
					continue;
				}
				const FVector Base(P.X, P.Y, Ground);
				const float TowerH = 8500.0f;
				Batch.Add(Cylinder, Base + FVector(0, 0, TowerH * 0.5f - 100.0f), FRotator::ZeroRotator, FVector(420, 420, TowerH), Paint, 0.0f, SurfConcrete);
				const FVector Hub = Base + FVector(0, 0, TowerH + 200.0f);
				Batch.Add(Cube, Hub + Facing.RotateVector(FVector(-150, 0, 0)), Facing, FVector(900, 330, 330), Paint, 0.0f, SurfConcrete);
				Batch.Add(Sphere, Hub + Facing.RotateVector(FVector(330, 0, 0)), Facing, FVector(300, 300, 300), Paint, 0.0f, SurfConcrete);
				const float Phase = Rand.Range(0.0f, 120.0f);
				for (int32 b = 0; b < 3; ++b)
				{
					const float Bd = Phase + b * 120.0f;
					const float Br = FMath::DegreesToRadians(Bd);
					const float L = 4200.0f;
					const FVector Offset = Facing.RotateVector(FVector(360.0f, -FMath::Sin(Br) * L * 0.5f, FMath::Cos(Br) * L * 0.5f));
					Batch.Add(Cube, Hub + Offset, FRotator(0.0f, WindYaw, Bd), FVector(40, 260, L), Paint, 0.0f, SurfConcrete);
				}
			}
		}
	});
}

namespace
{
	// A house seen from outside: walls with windows, roof, porch. Drops furniture, floors, carpet and interior partitions.
	bool KeepInShell(const HouseGen::FPiece& P, float HalfW, float HalfD)
	{
		if (P.bFurniture) return false;
		switch (P.Surface)
		{
		case HouseGen::ESurface::Carpet:
		case HouseGen::ESurface::PlankFloor:
		case HouseGen::ESurface::TileFloor:
			return false;
		case HouseGen::ESurface::InteriorWall:
			// Keep only the inner skin of exterior walls (near the perimeter), not room partitions.
			return FMath::Abs(FMath::Abs(P.Center.X) - HalfW) < 40.0f || FMath::Abs(FMath::Abs(P.Center.Y) - HalfD) < 40.0f;
		default:
			return true;
		}
	}
}

void ATerrainStreamer::AddRealBuildings(const FIntPoint& Coord, FPropBatch& Batch, EProps Level) const
{
	const FVector2D Min(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize());
	const FVector2D Max = Min + FVector2D(ChunkWorldSize());
	if (!RealPlace::Covers(Min, Max, 0.0))
	{
		return;
	}
	TArray<const RealPlace::FBuilding*> Buildings;
	RealPlace::BuildingsIn(Min, Max, Buildings);
	static const FLinearColor Washes[] = { FLinearColor(1.5f, 1.45f, 1.3f), FLinearColor(1.35f, 1.3f, 1.05f), FLinearColor(1.1f, 1.2f, 1.25f), FLinearColor(1.4f, 1.15f, 1.0f), FLinearColor(1.2f, 1.2f, 1.2f) };
	for (const RealPlace::FBuilding* B : Buildings)
	{
		// Smallest oriented rectangle around the outline: try each edge direction. Points are (X north, Y east).
		double BestArea = TNumericLimits<double>::Max();
		float BestAngleDeg = 0.0f, BestW = 0.0f, BestD = 0.0f;
		FVector2D BestCenter = B->Centroid;
		for (int32 i = 0; i < B->Outline.Num(); ++i)
		{
			const FVector2D Edge = (B->Outline[(i + 1) % B->Outline.Num()] - B->Outline[i]).GetSafeNormal();
			if (Edge.IsNearlyZero())
			{
				continue;
			}
			const FVector2D Perp(-Edge.Y, Edge.X);
			double LoU = 1e30, HiU = -1e30, LoV = 1e30, HiV = -1e30;
			for (const FVector2D& P : B->Outline)
			{
				const double U = FVector2D::DotProduct(P, Edge), V = FVector2D::DotProduct(P, Perp);
				LoU = FMath::Min(LoU, U);
				HiU = FMath::Max(HiU, U);
				LoV = FMath::Min(LoV, V);
				HiV = FMath::Max(HiV, V);
			}
			const double Area = (HiU - LoU) * (HiV - LoV);
			if (Area < BestArea)
			{
				BestArea = Area;
				BestW = HiU - LoU;
				BestD = HiV - LoV;
				// Rotator yaw turns world +X toward +Y; Edge is (X, Y).
				BestAngleDeg = FMath::RadiansToDegrees(FMath::Atan2(Edge.Y, Edge.X));
				BestCenter = Edge * ((LoU + HiU) * 0.5) + Perp * ((LoV + HiV) * 0.5);
			}
		}
		if (BestW < 300.0f || BestD < 300.0f)
		{
			continue;
		}
		const uint32 Seed = WorldGen::Hash(FMath::RoundToInt(B->Centroid.X / 100.0), FMath::RoundToInt(B->Centroid.Y / 100.0), 777);

		// Face the front door (+Y local) toward the nearest street; the width/depth axes follow.
		float Yaw = BestAngleDeg;
		float W = BestW, D = BestD;
		FVector2D RoadPoint;
		float RoadWidth = 0.0f;
		if (RealPlace::NearestRoad(B->Centroid, 6000.0, RoadPoint, RoadWidth))
		{
			const FVector2D To = (RoadPoint - B->Centroid).GetSafeNormal();
			float BestDot = -2.0f;
			int32 BestK = 0;
			for (int32 k = 0; k < 4; ++k)
			{
				const FVector Front = FRotator(0.0f, BestAngleDeg + 90.0f * k, 0.0f).RotateVector(FVector(0, 1, 0));
				const float Dot = FVector2D::DotProduct(FVector2D(Front.X, Front.Y), To);
				if (Dot > BestDot)
				{
					BestDot = Dot;
					BestK = k;
				}
			}
			Yaw = BestAngleDeg + 90.0f * BestK;
			// Turning the frame a quarter turn swaps which measured side is the width.
			if (BestK % 2 == 1)
			{
				Swap(W, D);
			}
		}

		const FVector2D Centre2 = BestCenter;
		const float Ground = WorldGen::Height(Centre2.X, Centre2.Y);
		if (Ground < 100.0f)
		{
			continue;
		}
		float High = Ground;
		for (const FVector2D Corner : { FVector2D(-1, -1), FVector2D(1, -1), FVector2D(-1, 1), FVector2D(1, 1) })
		{
			const FVector C = FVector(Centre2.X, Centre2.Y, 0.0) + FRotator(0.0f, Yaw, 0.0f).RotateVector(FVector(Corner.X * W * 0.5f, Corner.Y * D * 0.5f, 0.0f));
			High = FMath::Max(High, WorldGen::Height(C.X, C.Y));
		}
		const bool bChurch = B->Type == TEXT("church") || B->Type == TEXT("cathedral") || B->Type == TEXT("chapel");
		const bool bBig = B->Type == TEXT("retail") || B->Type == TEXT("commercial") || B->Type == TEXT("industrial") || B->Type == TEXT("warehouse")
			|| B->Type == TEXT("school") || B->Type == TEXT("civic") || B->Type == TEXT("public") || B->Type == TEXT("hospital")
			|| B->Type == TEXT("office") || B->Type == TEXT("apartments") || W * D > 1.6e7f;
		int32 Floors = 1;
		if (B->HeightM > 3.0f)
		{
			Floors = FMath::Clamp(FMath::RoundToInt((B->HeightM - 1.5f) / 3.3f), 1, 12);
		}
		else if (!bBig)
		{
			Floors = (Seed % 10) < 4 ? 2 : 1;
		}
		else if (B->Type == TEXT("school") || B->Type == TEXT("apartments"))
		{
			Floors = 2;
		}
		const HouseGen::EStyle Style = (Seed & 3) == 0 ? HouseGen::EStyle::Brick : HouseGen::EStyle::Plaster;
		const FQuat Quat = FRotator(0.0f, Yaw, 0.0f).Quaternion();
		const FVector Base(Centre2.X, Centre2.Y, High + 40.0f);
		const bool bDetailed = W * D <= 1.2e7f;
		if (Level >= EProps::Shell && (bDetailed || bChurch))
		{
			const HouseGen::FHouse Plan = bChurch ? HouseGen::GenerateChurch(Seed, Style)
				: HouseGen::Generate(Seed, Style, Floors > 1, W, D, bBig, Floors);
			const FLinearColor Wash = Washes[Seed % UE_ARRAY_COUNT(Washes)];
			for (const HouseGen::FPiece& Piece : Plan.Pieces)
			{
				if (Level != EProps::Near && !KeepInShell(Piece, Plan.Width * 0.5f, Plan.Depth * 0.5f)) continue;
				const FVector World = Base + Quat.RotateVector(Piece.Center);
				const FRotator PieceRot = (Quat * Piece.Rotation.Quaternion()).Rotator();
				if (Piece.bFurniture)
				{
					Batch.Add(static_cast<EPropPart>(Furniture0 + static_cast<int32>(Piece.Furniture)), World, PieceRot, FVector::OneVector, White);
					continue;
				}
				FVector Size = Piece.Size;
				FVector Mid = World;
				if (Piece.Surface == HouseGen::ESurface::Stone && Piece.Center.Z < 0.0f)
				{
					const float Extra = High - Ground + 150.0f;
					Size.Z += Extra;
					Mid.Z -= Extra * 0.5f;
				}
				Batch.Add(static_cast<EPropPart>(BuildSurf0 + static_cast<int32>(Piece.Surface)), Mid, PieceRot, Size,
					Piece.Surface == HouseGen::ESurface::ExteriorWall ? Wash : White);
			}
			continue;
		}
		// Far away (or too big for a furnished interior): a solid block, plus a roof on houses.
		const float Height = Floors * 290.0f + (bBig ? 80.0f : 120.0f);
		Batch.Add(Cube, Base + Quat.RotateVector(FVector(0, 0, Height * 0.5f - 60.0f)), FRotator(0.0f, Yaw, 0.0f), FVector(W, D, Height + 60.0f + (High - Ground)),
			bBig ? FLinearColor(1.1f, 1.05f, 0.95f) : Washes[Seed % UE_ARRAY_COUNT(Washes)], 0.0f, SurfConcrete);
		if (!bBig)
		{
			Batch.Add(Cube, Base + Quat.RotateVector(FVector(0, 0, Height + 40.0f)), FRotator(0.0f, Yaw, 0.0f), FVector(W + 60.0f, D + 60.0f, 120.0f), FLinearColor(0.35f, 0.32f, 0.3f), 0.0f, SurfSlate);
		}
	}

	// Infill houses on residential lots the map doesn't show.
	TArray<const RealPlace::FLot*> Lots;
	RealPlace::LotsIn(Min, Max, Lots);
	for (const RealPlace::FLot* Lot : Lots)
	{
		const float Ground = WorldGen::Height(Lot->Pos.X, Lot->Pos.Y);
		if (Ground < 100.0f)
		{
			continue;
		}
		const float W = Lot->Width, D = Lot->Depth;
		float High = Ground;
		for (const FVector2D Corner : { FVector2D(-1, -1), FVector2D(1, -1), FVector2D(-1, 1), FVector2D(1, 1) })
		{
			const FVector C = FVector(Lot->Pos.X, Lot->Pos.Y, 0.0) + FRotator(0.0f, Lot->Yaw, 0.0f).RotateVector(FVector(Corner.X * W * 0.5f, Corner.Y * D * 0.5f, 0.0f));
			High = FMath::Max(High, WorldGen::Height(C.X, C.Y));
		}
		const int32 Floors = (Lot->Seed % 10) < 4 ? 2 : 1;
		const HouseGen::EStyle Style = (Lot->Seed & 3) == 0 ? HouseGen::EStyle::Brick : HouseGen::EStyle::Plaster;
		const FQuat Quat = FRotator(0.0f, Lot->Yaw, 0.0f).Quaternion();
		const FVector Base(Lot->Pos.X, Lot->Pos.Y, High + 40.0f);
		const FLinearColor Wash = Washes[Lot->Seed % UE_ARRAY_COUNT(Washes)];
		if (Level >= EProps::Shell || (Lot->bFamilyHouse && Level >= EProps::Full))
		{
			const HouseGen::FHouse Plan = Lot->bFamilyHouse ? HouseGen::GenerateFamilyHouse1719() : HouseGen::Generate(Lot->Seed, Style, Floors > 1, W, D, false, Floors);
			if (Lot->bFamilyHouse)
			{
				// Log each furniture model's footprint, and any pieces of furniture whose footprints overlap on the same floor.
				struct FFoot { int32 Type; FVector2D C; double Yaw; FVector2D Half; double Z; };
				TArray<FFoot> Feet;
				for (const HouseGen::FPiece& Piece : Plan.Pieces)
				{
					if (!Piece.bFurniture) continue;
					const FBox& Bx = PartBounds[Furniture0 + static_cast<int32>(Piece.Furniture)];
					const FVector Size = Bx.GetSize();
					Feet.Add({ static_cast<int32>(Piece.Furniture), FVector2D(Piece.Center.X, Piece.Center.Y), Piece.Rotation.Yaw, FVector2D(Size.X, Size.Y) * 0.5f, Piece.Center.Z });
				}
				static bool bSizesLogged = false;
				if (!bSizesLogged)
				{
					bSizesLogged = true;
					for (int32 t = 0; t < static_cast<int32>(HouseGen::EFurniture::Count); ++t)
					{
						const FVector Size = PartBounds[Furniture0 + t].GetSize();
						UE_LOG(LogTemp, Display, TEXT("FurnitureSize type=%d %.0f x %.0f x %.0f cm"), t, Size.X, Size.Y, Size.Z);
					}
				}
				auto Corners = [](const FFoot& F, FVector2D Out[4])
				{
					const float C = FMath::Cos(FMath::DegreesToRadians(F.Yaw)), Sn = FMath::Sin(FMath::DegreesToRadians(F.Yaw));
					const FVector2D Ax(C, Sn), Ay(-Sn, C);
					Out[0] = F.C + Ax * F.Half.X + Ay * F.Half.Y; Out[1] = F.C - Ax * F.Half.X + Ay * F.Half.Y;
					Out[2] = F.C - Ax * F.Half.X - Ay * F.Half.Y; Out[3] = F.C + Ax * F.Half.X - Ay * F.Half.Y;
				};
				auto Overlap = [&](const FFoot& A, const FFoot& B)
				{
					FVector2D PA[4], PB[4];
					Corners(A, PA); Corners(B, PB);
					const FVector2D Axes[4] = { PA[0] - PA[1], PA[0] - PA[3], PB[0] - PB[1], PB[0] - PB[3] };
					for (const FVector2D& Ax0 : Axes)
					{
						const FVector2D Ax = Ax0.GetSafeNormal();
						float MinA = 1e9f, MaxA = -1e9f, MinB = 1e9f, MaxB = -1e9f;
						for (int32 k = 0; k < 4; ++k)
						{
							const float DA = FVector2D::DotProduct(PA[k], Ax), DB = FVector2D::DotProduct(PB[k], Ax);
							MinA = FMath::Min(MinA, DA); MaxA = FMath::Max(MaxA, DA); MinB = FMath::Min(MinB, DB); MaxB = FMath::Max(MaxB, DB);
						}
						if (MaxA < MinB + 2.0f || MaxB < MinA + 2.0f) return false;
					}
					return true;
				};
				for (int32 i = 0; i < Feet.Num(); ++i)
					for (int32 j = i + 1; j < Feet.Num(); ++j)
						if (FMath::Abs(Feet[i].Z - Feet[j].Z) < 150.0f && Overlap(Feet[i], Feet[j]))
							UE_LOG(LogTemp, Display, TEXT("FurnitureOverlap types %d and %d at (%.0f,%.0f) and (%.0f,%.0f) z=%.0f"), Feet[i].Type, Feet[j].Type, Feet[i].C.X, Feet[i].C.Y, Feet[j].C.X, Feet[j].C.Y, Feet[i].Z);
			}
			for (const HouseGen::FPiece& Piece : Plan.Pieces)
			{
				// Only the viewer's own chunk gets furnished interiors (the family's house always does); the rest are shells.
				if (Level != EProps::Near && !Lot->bFamilyHouse && !KeepInShell(Piece, Plan.Width * 0.5f, Plan.Depth * 0.5f)) continue;
				const FVector World = Base + Quat.RotateVector(Piece.Center);
				const FRotator PieceRot = (Quat * Piece.Rotation.Quaternion()).Rotator();
				if (Piece.bFurniture)
				{
					Batch.Add(static_cast<EPropPart>(Furniture0 + static_cast<int32>(Piece.Furniture)), World, PieceRot, FVector::OneVector, White);
					continue;
				}
				FVector Size = Piece.Size;
				FVector Mid = World;
				if (Piece.Surface == HouseGen::ESurface::Stone && Piece.Center.Z < 0.0f)
				{
					const float Extra = High - Ground + 150.0f;
					Size.Z += Extra;
					Mid.Z -= Extra * 0.5f;
				}
				FLinearColor Tint = Piece.Surface == HouseGen::ESurface::ExteriorWall ? Wash : White;
				if (Lot->bFamilyHouse && !Piece.bTinted)
				{
					// Blue vinyl siding outside, earth-tone paint inside.
					if (Piece.Surface == HouseGen::ESurface::ExteriorWall) Tint = FLinearColor(0.42f, 0.62f, 1.25f);
					else if (Piece.Surface == HouseGen::ESurface::InteriorWall) Tint = FLinearColor(1.25f, 1.02f, 0.78f);
				}
				if (Piece.bTinted) Tint = Piece.Tint;
				Batch.Add(static_cast<EPropPart>(BuildSurf0 + static_cast<int32>(Piece.Surface)), Mid, PieceRot, Size, Tint);
			}
			// The single tree in the back yard.
			if (Lot->bFamilyHouse && NumLoadedScanned > 0 && MatureSlots.Num() > 0)
			{
				const FVector Tree = Base + Quat.RotateVector(FVector(-330.0f, -1450.0f, 0.0f));
				const float TreeGround = WorldGen::Height(Tree.X, Tree.Y);
				Batch.Add(static_cast<EPropPart>(Scanned0 + MatureSlots[0]), FVector(Tree.X, Tree.Y, TreeGround - 25.0f), FRotator(0.0f, 40.0f, 0.0f), FVector(0.9f), White);
				Batch.Add(Trunk, FVector(Tree.X, Tree.Y, TreeGround + ScannedTreeHeight[MatureSlots[0]] * 0.45f), FRotator::ZeroRotator, FVector(120.0f, 120.0f, ScannedTreeHeight[MatureSlots[0]] * 0.9f), White);
			}
		}
		else
		{
			const float Height = Floors * 290.0f + 120.0f;
			Batch.Add(Cube, Base + Quat.RotateVector(FVector(0, 0, Height * 0.5f - 60.0f)), FRotator(0.0f, Lot->Yaw, 0.0f), FVector(W, D, Height + 60.0f + (High - Ground)), Wash, 0.0f, SurfConcrete);
			Batch.Add(Cube, Base + Quat.RotateVector(FVector(0, 0, Height + 40.0f)), FRotator(0.0f, Lot->Yaw, 0.0f), FVector(W + 60.0f, D + 60.0f, 120.0f), FLinearColor(0.35f, 0.32f, 0.3f), 0.0f, SurfSlate);
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

			constexpr int32 Blocks = 11;
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
					const float H = FMath::Lerp(2500.0f, 38000.0f, Falloff * Falloff * FMath::Pow(Tower.Next(), 0.7f));
					const float W = Tower.Range(1800.0f, 3400.0f);
					const float Dp = W * Tower.Range(0.7f, 1.0f);
					const int32 Style = FMath::FloorToInt(Tower.Next() * 3.0f); // 0 curtain glass, 1 ribbon windows, 2 masonry grid
					const FLinearColor Concrete = Jitter(FLinearColor(1.25f, 1.2f, 1.12f), Tower.Next(), 0.15f);
					const FLinearColor Glass = Style == 0 ? FLinearColor(0.55f, 0.7f, 0.85f) : FLinearColor(0.35f, 0.42f, 0.5f);
					const FRotator Rot(0, Tower.Range(-4.0f, 4.0f), 0);
					const FVector Base(Pos.X, Pos.Y, Ground - 200.0f);
					auto Part = [&](EPropPart Shape, const FVector& Local, const FVector& Size, const FLinearColor& Color, ESurface Surface, float Glow = 0.0f)
					{
						Batch.Add(Shape, Base + Rot.RotateVector(Local), Rot, Size, Color, Glow, Surface);
					};

					// Podium: a wider two-storey base with a glazed shopfront band.
					Part(Cube, FVector(0, 0, 450.0f), FVector(W + 600.0f, Dp + 600.0f, 900.0f), Concrete, SurfConcrete);
					Part(Cube, FVector(0, 0, 420.0f), FVector(W + 620.0f, Dp + 620.0f, 380.0f), Glass, SurfGlass);

					// Shaft: glass core with a floor slab (spandrel) band every storey; masonry adds vertical piers.
					const float ShaftH = H - 900.0f;
					Part(Cube, FVector(0, 0, 900.0f + ShaftH * 0.5f), FVector(W, Dp, ShaftH), Glass, SurfGlass);
					constexpr float Floor = 380.0f;
					const float Band = Style == 0 ? 35.0f : Style == 1 ? 140.0f : 110.0f;
					for (float Z = 900.0f + Floor; Z < H - 150.0f; Z += Floor)
					{
						Part(Cube, FVector(0, 0, Z), FVector(W + 30.0f, Dp + 30.0f, Band), Style == 0 ? FLinearColor(0.3f, 0.32f, 0.35f) : Concrete, Style == 0 ? SurfGlass : SurfConcrete);
					}
					if (Style == 2)
					{
						const int32 Piers = FMath::Max(3, FMath::FloorToInt(W / 320.0f));
						for (int32 k = 0; k <= Piers; ++k)
						{
							const float T = -0.5f + static_cast<float>(k) / Piers;
							Part(Cube, FVector(T * W, 0, 900.0f + ShaftH * 0.5f), FVector(80, Dp + 36.0f, ShaftH), Concrete, SurfConcrete);
							Part(Cube, FVector(0, T * Dp, 900.0f + ShaftH * 0.5f), FVector(W + 36.0f, 80, ShaftH), Concrete, SurfConcrete);
						}
					}

					// Parapet, plant rooms, water tank and an aircraft warning light on the tallest.
					Part(Cube, FVector(0, 0, H + 60.0f), FVector(W + 40.0f, Dp + 40.0f, 120.0f), Concrete, SurfConcrete);
					Part(Cube, FVector(W * 0.12f, Dp * 0.1f, H + 300.0f), FVector(W * 0.45f, Dp * 0.35f, 480.0f), Concrete, SurfConcrete);
					Part(Cube, FVector(-W * 0.25f, -Dp * 0.2f, H + 180.0f), FVector(W * 0.2f, Dp * 0.25f, 240.0f), FLinearColor(0.9f, 0.9f, 0.92f), SurfConcrete);
					Part(Cylinder, FVector(W * 0.3f, -Dp * 0.25f, H + 260.0f), FVector(260, 260, 400), FLinearColor(0.6f, 0.45f, 0.35f), SurfConcrete);
					if (H > 12000.0f)
					{
						Part(Cylinder, FVector(0, 0, H + 900.0f), FVector(30, 30, 1200), FLinearColor(0.8f, 0.8f, 0.8f), SurfConcrete);
						Part(Sphere, FVector(0, 0, H + 1520.0f), FVector(90, 90, 90), FLinearColor(1.0f, 0.1f, 0.05f), SurfGlass, 40.0f);
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

			// The island itself: a generated mesh (radius 100 units) scaled to size, with a small wood on top.
			const int32 Variant = FMath::FloorToInt(Rand.Next() * NumIslandVariants) % NumIslandVariants;
			const float Scale = R / 100.0f;
			const FRotator Spin(0.0f, Rand.Range(0.0f, 360.0f), 0.0f);
			Batch.Add(static_cast<EPropPart>(Island0 + Variant), Top, Spin, FVector(Scale), White);

			const FTreeTemplate& T = TreeTemplates[TreeBroadleafA];
			const int32 Trees = 10 + FMath::FloorToInt(Rand.Next() * 20);
			for (int32 i = 0; i < Trees; ++i)
			{
				const float A = Rand.Range(0.0f, 2.0f * PI);
				const float D = 70.0f * FMath::Sqrt(Rand.Next());
				const float LX = FMath::Cos(A) * D, LY = FMath::Sin(A) * D;
				const FVector Base = Top + Spin.RotateVector(FVector(LX, LY, IslandTopHeight(Variant, LX, LY)) * Scale) - FVector(0, 0, 30.0f);
				const float Size = Rand.Range(0.8f, 1.4f);
				const FRotator TreeSpin(0.0f, Rand.Range(0.0f, 360.0f), 0.0f);
				Batch.Add(IslandTree, Base, TreeSpin, FVector(Size), BarkWarm);
				const FLinearColor Leaves = Jitter(LeafTemperate, Rand.Next());
				for (const FTreeTemplate::FTip& Tip : T.Tips)
				{
					const float W = Tip.Size * Size;
					Batch.Add(IslandBush, Base + TreeSpin.RotateVector(Tip.Position * Size), FRotator(0.0f, Rand.Range(0.0f, 360.0f), 0.0f), FVector(W, W, W * 0.85f), Leaves);
				}
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

UInstancedStaticMeshComponent* ATerrainStreamer::AcquirePart(EPropPart Part)
{
	UInstancedStaticMeshComponent* Component = PartPools[Part].Num() > 0 ? PartPools[Part].Pop(EAllowShrinking::No) : nullptr;
	if (!Component)
	{
		// Nanite culls and LODs each instance on the GPU, so plain instancing is cheaper there than a
		// hierarchical component, whose CPU culling tree would have to be rebuilt for every chunk.
		if (Part >= Scanned0)
		{
			Component = NewObject<UInstancedStaticMeshComponent>(this);
		}
		else
		{
			Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
		}
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(PartMeshes[Part]);
		if (PartMaterials[Part])
		{
			Component->SetMaterial(0, PartMaterials[Part]);
		}
		// Trunk colliders, rocks and buildings are solid so you can weave between trees; foliage and the
		// tree wood meshes themselves are not (the colliders stand in for trunks). Vegetation dissolves
		// in and out smoothly by distance in its materials, so there's no hard cull distance here.
		if (Part == Bush || Part == IslandBush || Part >= Furniture0 || Part == BuildSurf0 + static_cast<int32>(HouseGen::ESurface::Glass))
		{
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		else
		{
			Component->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		if (Part >= Scanned0)
		{
			// The packs' wind animation (world position offset) distorts on scaled Nanite instances.
			Component->SetEvaluateWorldPositionOffset(false);
		}
		if (Part == Trunk)
		{
			Component->ComponentTags.Add(TEXT("TreeTrunk"));
			Component->SetHiddenInGame(true);
			Component->SetCastShadow(false);
		}
		Component->NumCustomDataFloats = 5;
		Component->SetupAttachment(RootComponent);
		Component->RegisterComponent();
		AllParts.Add(Component);
	}
	Component->SetVisibility(true);
	return Component;
}

void ATerrainStreamer::ReleaseChunk(FChunk& Chunk)
{
	for (UProceduralMeshComponent** Mesh : { &Chunk.Mesh, &Chunk.PathMesh })
	{
		if (*Mesh)
		{
			(*Mesh)->ClearAllMeshSections();
			(*Mesh)->SetVisibility(false);
			MeshPool.Add(*Mesh);
			*Mesh = nullptr;
		}
	}
	for (int32 Part = 0; Part < NumParts; ++Part)
	{
		if (UInstancedStaticMeshComponent* Component = Chunk.Parts[Part])
		{
			Component->ClearInstances();
			Component->SetVisibility(false);
			PartPools[Part].Add(Component);
			Chunk.Parts[Part] = nullptr;
		}
	}
}
