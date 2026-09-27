#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "ExplorerGameMode.generated.h"

UCLASS()
class EXPLORER_API AExplorerGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AExplorerGameMode();

protected:
	virtual void BeginPlay() override;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Flight")
	TSubclassOf<class APawn> FlightPawnClass;
};
