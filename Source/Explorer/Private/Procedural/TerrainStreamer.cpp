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
	constexpr double CityRadius = 70000.0;
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
	UE_LOG(LogTemp, Log, TEXT("Loaded %d scanned tree meshes"), NumLoadedScanned);
	for (int32 Variant = 0; Variant < NumIslandVariants; ++Variant)
	{
		PartMeshes[Island0 + Variant] = CreateIslandMesh(Variant);
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
		const FVector3f Out(FMath::Cos(A), FMath::Sin(A), 0.0f);
		const float Reach = Radius * Flare * Range(2.0f, 3.0f);
		Points = { FVector3f(0, 0, Radius * 1.4f), Out * Reach * 0.35f + FVector3f(0, 0, Radius * 0.4f), Out * Reach + FVector3f(0, 0, -Radius * 0.4f) };
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
			const EProps Props = DistSq <= DetailRadius * DetailRadius ? EProps::Full
				: (NumLoadedScanned > 0 && DistSq <= TreeRadius * TreeRadius) ? EProps::Trees : EProps::Landmarks;

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

	// Solid props only need physics right around the viewer; creating bodies for every trunk in the
	// whole detail ring is what makes streaming slow.
	for (TPair<FIntPoint, FChunk>& Pair : Chunks)
	{
		const bool bWant = (Pair.Key - Center).SizeSquared() <= CollisionRadius * CollisionRadius;
		if (Pair.Value.bPropCollision != bWant)
		{
			for (int32 Part = 0; Part < Tree0; ++Part)
			{
				if (Part != Bush && Part != IslandBush && Pair.Value.Parts[Part])
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
	TArray<WorldGen::FPath> Paths;
	WorldGen::PathsNear(Origin, Origin + FVector2D(GrassTileSize), 500.0, Paths);

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
			float PathWidth;
			if (Paths.Num() > 0 && WorldGen::DistanceToPath(Paths, P, PathWidth) < PathWidth * 0.5f + 40.0f)
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
		SetInstances(Component, Transforms, Data);
	}
}

void ATerrainStreamer::SetInstances(UHierarchicalInstancedStaticMeshComponent* Component, const TArray<FTransform>& Transforms, const TArray<float>& CustomData)
{
	// Add all instances and write their custom data in one go, then build the culling tree once
	// (per-instance SetCustomData calls each trigger work in the hierarchical component).
	Component->ClearInstances();
	Component->AddInstances(Transforms, false, true);
	if (Component->PerInstanceSMCustomData.Num() == CustomData.Num())
	{
		FMemory::Memcpy(Component->PerInstanceSMCustomData.GetData(), CustomData.GetData(), CustomData.Num() * sizeof(float));
	}
	Component->BuildTreeIfOutdated(true, true);
	Component->MarkRenderStateDirty();
}

void ATerrainStreamer::BuildTerrain(const FIntPoint& Coord, FChunk& Chunk, int32 Step, bool bWithCollision)
{
	if (!Chunk.Mesh)
	{
		Chunk.Mesh = AcquireMesh();
	}
	BuildSurface(Chunk.Mesh, FVector(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize(), 0.0), ChunkResolution / Step, GridSpacing * Step, bWithCollision);
	// Paths sit exactly on full-detail terrain, so they exist only there.
	if (Step == 1 && Chunk.Step != 1)
	{
		BuildPaths(Coord, Chunk);
	}
	else if (Step != 1 && Chunk.PathMesh)
	{
		Chunk.PathMesh->ClearAllMeshSections();
		Chunk.PathMesh->SetVisibility(false);
		MeshPool.Add(Chunk.PathMesh);
		Chunk.PathMesh = nullptr;
	}
	Chunk.Step = Step;
	Chunk.bHasCollision = bWithCollision;
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

void ATerrainStreamer::BuildPaths(const FIntPoint& Coord, FChunk& Chunk)
{
	const FVector2D Min(Coord.X * ChunkWorldSize(), Coord.Y * ChunkWorldSize());
	const FVector2D Max = Min + FVector2D(ChunkWorldSize());
	TArray<WorldGen::FPath> Paths;
	WorldGen::PathsNear(Min, Max, 1000.0, Paths);

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> Colors;
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
			const int32 Base = Vertices.Num();
			for (const float Edge : { -0.5f, 0.5f })
			{
				const FVector2D Q = P + Side * Path.Width * Edge;
				Vertices.Add(FVector(Q.X - Min.X, Q.Y - Min.Y, SurfaceHeight(Q.X, Q.Y) + 8.0f));
				Normals.Add(FVector::UpVector);
				UVs.Add(FVector2D(Edge + 0.5f, T * Length / 400.0f));
				Colors.Add(Path.bRoad ? FColor(255, 0, 0, 255) : FColor(0, 0, 0, 255));
			}
			if (Prev != INDEX_NONE)
			{
				Triangles.Append({ Prev, Base, Prev + 1, Prev + 1, Base, Base + 1 });
				Triangles.Append({ Prev, Prev + 1, Base, Prev + 1, Base + 1, Base }); // both faces, winding-proof
			}
			Prev = Base;
		}
	}

	if (Vertices.Num() == 0)
	{
		return;
	}
	if (!Chunk.PathMesh)
	{
		Chunk.PathMesh = AcquireMesh();
	}
	Chunk.PathMesh->SetWorldLocation(FVector(Min.X, Min.Y, 0.0));
	Chunk.PathMesh->CreateMeshSection(0, Vertices, Triangles, Normals, UVs, Colors, TArray<FProcMeshTangent>(), false);
	Chunk.PathMesh->SetMaterial(0, PathMaterial);
	Chunk.PathMesh->SetCastShadow(false);
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
	if (Level == EProps::Trees)
	{
		AddVegetation(Coord, Batch, true);
	}
	if (Level == EProps::Full)
	{
		AddVegetation(Coord, Batch, false);
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
			// Trees are placed at their base with a uniform scale in Size.
			if (Part >= Tree0)
			{
				Transforms.Add(FTransform(Instance.Rotation, Instance.Center, Instance.Size));
				continue;
			}
			const FVector Scale = Instance.Size / MeshSize;
			const FVector Location = Instance.Center - Instance.Rotation.RotateVector(Bounds.GetCenter() * Scale);
			Transforms.Add(FTransform(Instance.Rotation, Location, Scale));
		}

		if (!Component)
		{
			Component = AcquirePart(static_cast<EPropPart>(Part));
		}
		// Start without physics; Tick enables it if this chunk is close enough.
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SetInstances(Component, Transforms, Batch.CustomData[Part]);
	}
	Chunk.Props = Level;
	Chunk.bPropCollision = false;
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
					const int32 Slot = FMath::Min(FMath::FloorToInt(Tint * NumLoadedScanned), NumLoadedScanned - 1);
					// Scanned at real size (up to ~48 m tall); vary a little, and sink the root flare into the ground.
					const float Scale = FMath::Lerp(0.6f, 1.0f, WorldGen::HashFloat(CX, CY, SeedTreeSize));
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
				const bool bTwoStorey = !bAdobe && House.Next() < 0.35f;
				const float H = bTwoStorey ? House.Range(620.0f, 720.0f) : House.Range(360.0f, 420.0f);
				FLinearColor Wall = Plaster[FMath::FloorToInt(House.Next() * 3) % 3];
				ESurface WallSurface = House.Next() < 0.4f ? SurfBrick : SurfConcrete;
				if (bTimber) { Wall = FLinearColor(0.5f, 0.36f, 0.25f); WallSurface = SurfConcrete; }
				if (bAdobe) { Wall = FLinearColor(1.5f, 1.2f, 0.85f); WallSurface = SurfConcrete; }
				if (WallSurface == SurfBrick) { Wall = White; }

				// Everything is laid out in the house's own frame: X along its length, Y across, Z up from the ground.
				const FRotator Rot(0, Yaw, 0);
				const FVector Base(Pos.X, Pos.Y, Ground);
				auto Part = [&](EPropPart Shape, const FVector& Local, const FVector& Size, const FLinearColor& Color, ESurface Surface, float Roll = 0.0f)
				{
					Batch.Add(Shape, Base + Rot.RotateVector(Local), FRotator(0, Yaw, Roll), Size, Color, 0.0f, Surface);
				};
				const FLinearColor Stone(1.1f, 1.05f, 0.98f);
				const FLinearColor Trim(1.8f, 1.8f, 1.75f);
				const FLinearColor Glass(0.35f, 0.4f, 0.45f);
				const FLinearColor Wood(0.45f, 0.3f, 0.2f);

				// Stone plinth the house sits on (it also hides uneven ground), then the walls.
				Part(Cube, FVector(0, 0, -20.0f), FVector(W + 40.0f, D + 40.0f, 110.0f), Stone, SurfRock);
				Part(Cube, FVector(0, 0, H * 0.5f + 30.0f), FVector(W, D, H), Jitter(Wall, House.Next(), 0.06f), WallSurface);

				// Framed windows on every wall, one row per storey; a panelled door on the front.
				const int32 Storeys = bTwoStorey ? 2 : 1;
				const int32 AlongLong = FMath::Max(2, FMath::FloorToInt(W / 280.0f));
				const int32 AlongShort = FMath::Max(1, FMath::FloorToInt(D / 320.0f));
				const int32 DoorSlot = AlongLong / 2;
				auto Window = [&](const FVector& Center, bool bFacingY)
				{
					const FVector Out = bFacingY ? FVector(0, FMath::Sign(Center.Y), 0) : FVector(FMath::Sign(Center.X), 0, 0);
					const FVector FrameSize = bFacingY ? FVector(125, 14, 150) : FVector(14, 125, 150);
					const FVector GlassSize = bFacingY ? FVector(100, 16, 122) : FVector(16, 100, 122);
					Part(Cube, Center + Out * 6.0f, FrameSize, bAdobe ? Wood : Trim, SurfConcrete);
					Part(Cube, Center + Out * 9.0f, GlassSize, Glass, SurfGlass);
					Part(Cube, Center + Out * 14.0f - FVector(0, 0, 80.0f), bFacingY ? FVector(140, 24, 12) : FVector(24, 140, 12), bAdobe ? Wood : Trim, SurfConcrete);
				};
				for (int32 Storey = 0; Storey < Storeys; ++Storey)
				{
					const float Z = 30.0f + (Storey + 0.55f) * H / Storeys;
					for (const float Side : { -1.0f, 1.0f })
					{
						for (int32 k = 0; k < AlongLong; ++k)
						{
							if (Storey == 0 && Side > 0 && k == DoorSlot)
							{
								continue;
							}
							Window(FVector(-W * 0.5f + (k + 0.5f) * W / AlongLong, Side * D * 0.5f, Z), true);
						}
						for (int32 k = 0; k < AlongShort; ++k)
						{
							Window(FVector(Side * W * 0.5f, -D * 0.5f + (k + 0.5f) * D / AlongShort, Z), false);
						}
					}
				}
				const float DoorX = -W * 0.5f + (DoorSlot + 0.5f) * W / AlongLong;
				Part(Cube, FVector(DoorX, D * 0.5f + 6.0f, 30.0f + 110.0f), FVector(120, 14, 230), bAdobe ? Wood : Trim, SurfConcrete);
				Part(Cube, FVector(DoorX, D * 0.5f + 9.0f, 30.0f + 105.0f), FVector(96, 16, 210), Wood, SurfConcrete);
				Part(Cube, FVector(DoorX, D * 0.5f + 60.0f, 20.0f), FVector(180, 110, 30), Stone, SurfRock);

				if (bAdobe)
				{
					// Flat roof with a parapet and roof beams (vigas) poking out of the walls.
					Part(Cube, FVector(0, 0, H + 30.0f + 25.0f), FVector(W + 20.0f, D + 20.0f, 50.0f), Jitter(Wall, House.Next(), 0.04f), SurfConcrete);
					const int32 Vigas = FMath::FloorToInt(W / 160.0f);
					for (int32 k = 0; k < Vigas; ++k)
					{
						Part(Cylinder, FVector(-W * 0.5f + (k + 0.5f) * W / Vigas, 0, H - 20.0f), FVector(22, D + 90.0f, 22), Wood, SurfConcrete, 90.0f);
					}
				}
				else
				{
					// Pitched roof with overhanging eaves (a box turned 45 degrees about the ridge), plus a chimney.
					const float S2 = D / UE_SQRT_2 + 70.0f;
					const FLinearColor Roof = bTimber ? FLinearColor(0.55f, 0.55f, 0.58f) : Roofs[FMath::FloorToInt(House.Next() * 3) % 3];
					Part(Cube, FVector(0, 0, H + 30.0f), FVector(W + 100.0f, S2, S2), Roof, SurfSlate, 45.0f);
					Part(Cube, FVector(0, 0, H + 30.0f), FVector(W + 10.0f, S2 - 20.0f, S2 - 20.0f), Jitter(Wall, House.Next(), 0.06f), WallSurface, 45.0f);
					const float ChimneyX = (House.Next() < 0.5f ? -1.0f : 1.0f) * W * 0.3f;
					Part(Cube, FVector(ChimneyX, D * 0.15f, H + 30.0f + D * 0.45f), FVector(70, 70, D * 0.6f), White, SurfBrick);
					Part(Cube, FVector(ChimneyX, D * 0.15f, H + 30.0f + D * 0.75f), FVector(90, 90, 20), Stone, SurfRock);
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

UHierarchicalInstancedStaticMeshComponent* ATerrainStreamer::AcquirePart(EPropPart Part)
{
	UHierarchicalInstancedStaticMeshComponent* Component = PartPools[Part].Num() > 0 ? PartPools[Part].Pop(EAllowShrinking::No) : nullptr;
	if (!Component)
	{
		Component = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(PartMeshes[Part]);
		if (PartMaterials[Part])
		{
			Component->SetMaterial(0, PartMaterials[Part]);
		}
		// Trunk colliders, rocks and buildings are solid so you can weave between trees; foliage and the
		// tree wood meshes themselves are not (the colliders stand in for trunks). Vegetation dissolves
		// in and out smoothly by distance in its materials, so there's no hard cull distance here.
		if (Part == Bush || Part == IslandBush || Part >= Tree0)
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
		if (UHierarchicalInstancedStaticMeshComponent* Component = Chunk.Parts[Part])
		{
			Component->ClearInstances();
			Component->SetVisibility(false);
			PartPools[Part].Add(Component);
			Chunk.Parts[Part] = nullptr;
		}
	}
}
