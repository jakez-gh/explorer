#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TerrainStreamer.generated.h"

class UProceduralMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;

/**
 * Endless procedural world. Keeps a disc of square chunks around the player: full detail and
 * collision up close, coarser terrain in the distance, trees and villages nearby, and large
 * landmarks (cities, floating islands) everywhere in view. Everything is a pure function of
 * world position (see WorldGen), so chunks line up seamlessly and places are always the same.
 */
UCLASS()
class EXPLORER_API ATerrainStreamer : public AActor
{
	GENERATED_BODY()

public:
	ATerrainStreamer();

	// Quads per chunk side at full detail.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain", meta = (ClampMin = "8"))
	int32 ChunkResolution = 64;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	float GridSpacing = 500.0f;

	// Radius, in chunks, of the disc kept loaded around the player.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Streaming")
	int32 ViewRadius = 17;

	// Chunks within this radius get trees, rocks and villages.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Streaming")
	int32 DetailRadius = 4;

	// Chunks within this radius get collision.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Streaming")
	int32 CollisionRadius = 2;

	// Milliseconds per frame spent building chunks (at least one chunk is always built).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Streaming")
	float BuildBudgetMs = 6.0f;

	// Terrain surface height in world space at a world XY position.
	UFUNCTION(BlueprintCallable, Category = "Terrain")
	float GetHeightAtLocation(FVector2D WorldXY) const;

	virtual void Tick(float DeltaTime) override;

	// Landmark placement, shared by generation and the -BiomeReport finder. Each returns false if the
	// region cell has none; otherwise the landmark's centre (islands: the top surface, in the air).
	static bool FindVillage(int32 CellX, int32 CellY, FVector& OutCenter);
	static bool FindCity(int32 CellX, int32 CellY, FVector& OutCenter);
	static bool FindFloatingIsland(int32 CellX, int32 CellY, FVector& OutTop, float& OutRadius);
	static double VillageCellSize();
	static double CityCellSize();
	static double IslandCellSize();

private:
	enum EPropPart : uint8
	{
		Cylinder,
		Cone,
		Sphere,
		Cube,
		NumParts
	};

	enum class EProps : uint8
	{
		None,
		Landmarks,
		Full,
	};

	struct FChunk
	{
		UProceduralMeshComponent* Mesh = nullptr;
		UHierarchicalInstancedStaticMeshComponent* Parts[NumParts] = {};
		int32 Step = 0;
		bool bHasCollision = false;
		EProps Props = EProps::None;
	};

	/** Instances collected for one chunk before they're pushed to its components. */
	struct FPropBatch
	{
		TArray<FTransform> Transforms[NumParts];
		TArray<float> CustomData[NumParts];

		void Add(EPropPart Part, const FVector& Center, const FRotator& Rotation, const FVector& SizeCm, const FLinearColor& Color, float Glow = 0.0f);
	};

	float ChunkWorldSize() const { return ChunkResolution * GridSpacing; }
	FIntPoint WorldToChunk(const FVector& Location) const;
	int32 StepForDistance(float DistanceInChunks) const;

	void BuildTerrain(const FIntPoint& Coord, FChunk& Chunk, int32 Step, bool bWithCollision);
	void BuildProps(const FIntPoint& Coord, FChunk& Chunk, EProps Level);
	void AddVegetation(const FIntPoint& Coord, FPropBatch& Batch) const;
	void AddVillages(const FIntPoint& Coord, FPropBatch& Batch) const;
	void AddCities(const FIntPoint& Coord, FPropBatch& Batch) const;
	void AddFloatingIslands(const FIntPoint& Coord, FPropBatch& Batch) const;
	bool ChunkContains(const FIntPoint& Coord, const FVector& Location) const;

	void ReleaseChunk(FChunk& Chunk);
	UProceduralMeshComponent* AcquireMesh();
	UHierarchicalInstancedStaticMeshComponent* AcquirePart(EPropPart Part);

	TMap<FIntPoint, FChunk> Chunks;

	UPROPERTY(VisibleAnywhere, Category = "Terrain")
	TObjectPtr<UStaticMeshComponent> Ocean;

	UPROPERTY(EditAnywhere, Category = "Terrain|Assets")
	TObjectPtr<UMaterialInterface> TerrainMaterial;

	UPROPERTY(EditAnywhere, Category = "Terrain|Assets")
	TObjectPtr<UMaterialInterface> PropMaterial;

	// Engine basic shapes, indexed by EPropPart.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> PartMeshes;

	// Recycled components, so streaming doesn't churn UObjects.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UProceduralMeshComponent>> MeshPool;

	TArray<UHierarchicalInstancedStaticMeshComponent*> PartPools[NumParts];

	// Every prop component ever created, pooled or live; keeps them referenced for GC.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> AllParts;
};
