#include "Game/ExplorerGameMode.h"
#include "Flight/FlightPawn.h"
#include "GameFramework/PlayerController.h"

AExplorerGameMode::AExplorerGameMode()
{
	DefaultPawnClass = AFlightPawn::StaticClass();
}

void AExplorerGameMode::BeginPlay()
{
	Super::BeginPlay();
	UE_LOG(LogTemp, Warning, TEXT("ExplorerGameMode started"));
}
