#include "Procedural/SimpleTerrainActor.h"
#include "ProceduralMeshComponent.h"
#include "Engine/CollisionProfile.h"

ASimpleTerrainActor::ASimpleTerrainActor()
{
	PrimaryActorTick.bCanEverTick = false;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	TerrainMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("TerrainMesh"));
	TerrainMesh->SetupAttachment(RootComponent);
	TerrainMesh->bUseAsyncCooking = true;
	TerrainMesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
}

void ASimpleTerrainActor::BeginPlay()
{
	Super::BeginPlay();
	GenerateTerrain();
}

void ASimpleTerrainActor::GenerateTerrain()
{
	const int32 Side = GridSize + 1;
	const float HalfExtent = GridSize * GridSpacing * 0.5f;

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;

	Vertices.Reserve(Side * Side);
	Normals.Reserve(Side * Side);
	UVs.Reserve(Side * Side);
	Triangles.Reserve(GridSize * GridSize * 6);

	for (int32 Y = 0; Y < Side; ++Y)
	{
		for (int32 X = 0; X < Side; ++X)
		{
			const float Height = GetTerrainHeight(X, Y);
			Vertices.Add(FVector(X * GridSpacing - HalfExtent, Y * GridSpacing - HalfExtent, Height));

			// Central-difference normal from neighbouring heights.
			const float DX = GetTerrainHeight(X + 1, Y) - GetTerrainHeight(X - 1, Y);
			const float DY = GetTerrainHeight(X, Y + 1) - GetTerrainHeight(X, Y - 1);
			Normals.Add(FVector(-DX, -DY, 2.0f * GridSpacing).GetSafeNormal());

			// One UV tile per grid cell so the default world-grid material shows scale.
			UVs.Add(FVector2D(X, Y));
		}
	}

	// Same winding as UKismetProceduralMeshLibrary::CreateGridMeshWelded, so faces point up.
	for (int32 Y = 0; Y < GridSize; ++Y)
	{
		for (int32 X = 0; X < GridSize; ++X)
		{
			const int32 BottomLeft = Y * Side + X;
			const int32 BottomRight = BottomLeft + 1;
			const int32 TopLeft = BottomLeft + Side;
			const int32 TopRight = TopLeft + 1;

			Triangles.Add(BottomLeft);
			Triangles.Add(TopLeft);
			Triangles.Add(BottomRight);

			Triangles.Add(BottomRight);
			Triangles.Add(TopLeft);
			Triangles.Add(TopRight);
		}
	}

	TerrainMesh->CreateMeshSection(
		0,
		Vertices,
		Triangles,
		Normals,
		UVs,
		TArray<FColor>(),
		TArray<FProcMeshTangent>(),
		true
	);

	UE_LOG(LogTemp, Log, TEXT("Terrain generated with %d vertices"), Vertices.Num());
}

float ASimpleTerrainActor::GetHeightAtLocation(FVector2D WorldXY) const
{
	const FVector Local = GetActorTransform().InverseTransformPosition(FVector(WorldXY, 0.0f));
	const float HalfExtent = GridSize * GridSpacing * 0.5f;
	const float GridX = (Local.X + HalfExtent) / GridSpacing;
	const float GridY = (Local.Y + HalfExtent) / GridSpacing;
	return GetActorLocation().Z + GetTerrainHeight(GridX, GridY);
}

float ASimpleTerrainActor::GetTerrainHeight(float GridX, float GridY) const
{
	// Fractal Brownian motion over Perlin noise, normalised to [0, 1].
	float Height = 0.0f;
	float Amplitude = 1.0f;
	float Frequency = NoiseScale;
	float MaxAmplitude = 0.0f;

	for (int32 i = 0; i < Octaves; ++i)
	{
		const FVector2D Sample = FVector2D(GridX, GridY) * Frequency + NoiseOffset * (i + 1);
		Height += FMath::PerlinNoise2D(Sample) * Amplitude;
		MaxAmplitude += Amplitude;

		Frequency *= 2.0f;
		Amplitude *= 0.5f;
	}

	Height = (Height / MaxAmplitude + 1.0f) * 0.5f;

	// Square to flatten valleys and sharpen peaks.
	return Height * Height * HeightMultiplier;
}
