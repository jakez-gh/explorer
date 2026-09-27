#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ExplorerGameMode.generated.h"

class ASimpleTerrainActor;

UCLASS()
class EXPLORER_API AExplorerGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AExplorerGameMode();

protected:
	virtual void BeginPlay() override;
	virtual APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot) override;

	// Spawned at the origin if the level doesn't already contain a terrain actor.
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Terrain")
	TSubclassOf<ASimpleTerrainActor> TerrainClass;

	// Height above the terrain surface at which the player starts.
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Flight")
	float StartAltitude = 5000.0f;

private:
	ASimpleTerrainActor* FindOrSpawnTerrain();
};
