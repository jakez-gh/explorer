#include "Game/ExplorerGameMode.h"
#include "Flight/FlightPawn.h"
#include "Procedural/SimpleTerrainActor.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"

AExplorerGameMode::AExplorerGameMode()
{
	DefaultPawnClass = AFlightPawn::StaticClass();
	TerrainClass = ASimpleTerrainActor::StaticClass();
}

void AExplorerGameMode::BeginPlay()
{
	Super::BeginPlay();

	// The engine's Basic template map ships a sky-sphere mesh (redundant with its SkyAtmosphere, and too small
	// to cover the view at flight altitude) and a floor that pokes through low terrain. Remove both.
	for (TActorIterator<AStaticMeshActor> It(GetWorld()); It; ++It)
	{
		const UStaticMesh* Mesh = It->GetStaticMeshComponent()->GetStaticMesh();
		if (Mesh && (Mesh->GetName() == TEXT("SM_SkySphere") || Mesh->GetName() == TEXT("SM_Template_Map_Floor")))
		{
			It->Destroy();
		}
	}

	UE_LOG(LogTemp, Log, TEXT("ExplorerGameMode started"));
}

APawn* AExplorerGameMode::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
	FVector Location = FVector::ZeroVector;
	if (const ASimpleTerrainActor* Terrain = FindOrSpawnTerrain())
	{
		Location = Terrain->GetActorLocation();
		Location.Z = Terrain->GetHeightAtLocation(FVector2D(Location));
	}
	Location.Z += StartAltitude;

	return SpawnDefaultPawnAtTransform(NewPlayer, FTransform(FRotator::ZeroRotator, Location));
}

ASimpleTerrainActor* AExplorerGameMode::FindOrSpawnTerrain()
{
	for (TActorIterator<ASimpleTerrainActor> It(GetWorld()); It; ++It)
	{
		return *It;
	}

	if (!TerrainClass)
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return GetWorld()->SpawnActor<ASimpleTerrainActor>(TerrainClass, FTransform::Identity, Params);
}
