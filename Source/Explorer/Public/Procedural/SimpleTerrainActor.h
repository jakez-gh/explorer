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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	int32 GridSize = 100;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	float GridSpacing = 200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	float NoiseScale = 0.01f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
	float HeightMultiplier = 1000.0f;

	UFUNCTION(BlueprintCallable, Category = "Terrain")
	void GenerateTerrain();

protected:
	virtual void BeginPlay() override;

private:
	float GetTerrainHeight(int32 X, int32 Y);
};
