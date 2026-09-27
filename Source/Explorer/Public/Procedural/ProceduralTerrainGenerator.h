#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralTerrainGenerator.generated.h"

UCLASS()
class EXPLORER_API AProceduralTerrainGenerator : public AActor
{
	GENERATED_BODY()

public:
	AProceduralTerrainGenerator();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	int32 TerrainSize = 2048;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	float TerrainScale = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	float NoiseScale = 0.01f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	float MaxHeight = 5000.0f;

	UFUNCTION(BlueprintCallable, Category = "Terrain")
	void GenerateTerrain();

protected:
	virtual void BeginPlay() override;

private:
	float PerlinNoise(float x, float y) const;
	float Smoothstep(float t) const;
};
