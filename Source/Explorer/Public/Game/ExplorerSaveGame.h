#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "ExplorerSaveGame.generated.h"

/**
 * Where the player was. The world itself is a pure function of position, so this is all that's
 * needed to pick up exactly where they left off.
 */
UCLASS()
class EXPLORER_API UExplorerSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	static constexpr const TCHAR* SlotName = TEXT("Explorer");

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	float Yaw = 0.0f;

	UPROPERTY()
	float Pitch = 0.0f;

	// Loads the save if there is one.
	static UExplorerSaveGame* Load();
	static void Save(const FVector& Location, float Yaw, float Pitch);
};
