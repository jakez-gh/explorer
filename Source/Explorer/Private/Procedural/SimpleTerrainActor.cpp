#include "Procedural/SimpleTerrainActor.h"
#include "ProceduralMeshComponent.h"
#include "Kismet/GameplayStatics.h"

ASimpleTerrainActor::ASimpleTerrainActor()
{
	PrimaryActorTick.bCanEverTick = false;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	TerrainMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("TerrainMesh"));
	TerrainMesh->SetupAttachment(RootComponent);
	TerrainMesh->bUseAsyncCooking = true;
	TerrainMesh->SetCollisionEnabled(ECC_WorldStatic);
}

void ASimpleTerrainActor::BeginPlay()
{
	Super::BeginPlay();
	GenerateTerrain();
}

void ASimpleTerrainActor::GenerateTerrain()
{
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;

	Vertices.Reserve((GridSize + 1) * (GridSize + 1));
	Triangles.Reserve(GridSize * GridSize * 6);

	for (int32 Y = 0; Y <= GridSize; ++Y)
	{
		for (int32 X = 0; X <= GridSize; ++X)
		{
			float Height = GetTerrainHeight(X, Y);
			FVector Position(X * GridSpacing, Y * GridSpacing, Height);
			Vertices.Add(Position);
			UVs.Add(FVector2D(X / (float)GridSize, Y / (float)GridSize));
		}
	}

	for (int32 Y = 0; Y < GridSize; ++Y)
	{
		for (int32 X = 0; X < GridSize; ++X)
		{
			int32 BottomLeft = Y * (GridSize + 1) + X;
			int32 BottomRight = BottomLeft + 1;
			int32 TopLeft = BottomLeft + (GridSize + 1);
			int32 TopRight = TopLeft + 1;

			Triangles.Add(BottomLeft);
			Triangles.Add(TopLeft);
			Triangles.Add(BottomRight);

			Triangles.Add(BottomRight);
			Triangles.Add(TopLeft);
			Triangles.Add(TopRight);
		}
	}

	Normals.SetNum(Vertices.Num());
	for (int32 i = 0; i < Normals.Num(); ++i)
	{
		Normals[i] = FVector::UpVector;
	}

	TerrainMesh->CreateMeshSection_Deprecated(
		0,
		Vertices,
		Triangles,
		Normals,
		UVs,
		TArray<FColor>(),
		TArray<FProcMeshTangent>(),
		true
	);

	UE_LOG(LogTemp, Warning, TEXT("Terrain generated with %d vertices"), Vertices.Num());
}

float ASimpleTerrainActor::GetTerrainHeight(int32 X, int32 Y)
{
	float Height = 0.0f;

	float Octaves = 3.0f;
	float Amplitude = 1.0f;
	float Frequency = 1.0f;
	float MaxAmplitude = 0.0f;

	for (float i = 0; i < Octaves; ++i)
	{
		float SampleX = (X * NoiseScale) * Frequency;
		float SampleY = (Y * NoiseScale) * Frequency;

		float Value = FMath::Sin(SampleX * 12.9898f + SampleY * 78.233f) * 43758.5453f;
		Value = FMath::Frac(Value);
		Value = Value * 2.0f - 1.0f;

		Height += Value * Amplitude;
		MaxAmplitude += Amplitude;

		Frequency *= 2.0f;
		Amplitude *= 0.5f;
	}

	Height /= MaxAmplitude;
	Height = (Height + 1.0f) * 0.5f;

	return Height * HeightMultiplier;
}
