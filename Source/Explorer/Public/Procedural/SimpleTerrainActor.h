#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SimpleTerrainActor.generated.h"

class UProceduralMeshComponent;

UCLASS()
class EXPLORER_API ASimpleTerrainActor : public AActor
{
	GENERATED_BODY()

public:
	ASimpleTerrainActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Terrain")
	UProceduralMeshComponent* TerrainMesh;

	// Vertices per side minus one. The terrain is centred on the actor.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	int32 GridSize = 256;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	float GridSpacing = 400.0f;

	// Noise frequency per grid cell for the first octave.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	float NoiseScale = 0.015f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	float HeightMultiplier = 25000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain", meta = (ClampMin = "1", ClampMax = "8"))
	int32 Octaves = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	FVector2D NoiseOffset = FVector2D(1234.5f, 678.9f);

	UFUNCTION(BlueprintCallable, Category = "Terrain")
	void GenerateTerrain();

	// Terrain surface height in world space at a world XY position.
	UFUNCTION(BlueprintCallable, Category = "Terrain")
	float GetHeightAtLocation(FVector2D WorldXY) const;

protected:
	virtual void BeginPlay() override;

private:
	// Height relative to the actor, sampled in (fractional) grid coordinates.
	float GetTerrainHeight(float GridX, float GridY) const;
};
