#include "Game/ExplorerSaveGame.h"
#include "Kismet/GameplayStatics.h"

UExplorerSaveGame* UExplorerSaveGame::Load()
{
	if (!UGameplayStatics::DoesSaveGameExist(SlotName, 0))
	{
		return nullptr;
	}
	return Cast<UExplorerSaveGame>(UGameplayStatics::LoadGameFromSlot(SlotName, 0));
}

void UExplorerSaveGame::Save(const FVector& Location, float Yaw, float Pitch)
{
	UExplorerSaveGame* SaveGame = Cast<UExplorerSaveGame>(UGameplayStatics::CreateSaveGameObject(StaticClass()));
	SaveGame->Location = Location;
	SaveGame->Yaw = Yaw;
	SaveGame->Pitch = Pitch;
	UGameplayStatics::SaveGameToSlot(SaveGame, SlotName, 0);
}
