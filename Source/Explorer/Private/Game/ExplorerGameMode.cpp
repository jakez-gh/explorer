#include "Game/ExplorerGameMode.h"
#include "Flight/FlightPawn.h"
#include "Game/ExplorerPlayerController.h"
#include "Game/ExplorerSaveGame.h"
#include "Procedural/TerrainStreamer.h"
#include "Procedural/WorldGen.h"
#include "Procedural/RealPlace.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"

AExplorerGameMode::AExplorerGameMode()
{
	DefaultPawnClass = AFlightPawn::StaticClass();
	PlayerControllerClass = AExplorerPlayerController::StaticClass();
	TerrainClass = ATerrainStreamer::StaticClass();
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

	SetupDreamAtmosphere();
	UE_LOG(LogTemp, Log, TEXT("ExplorerGameMode started"));
}

void AExplorerGameMode::SetupDreamAtmosphere()
{
	// Natural mid-afternoon sun: high enough for true colour, low enough for long shadows and relief.
	for (TActorIterator<ADirectionalLight> It(GetWorld()); It; ++It)
	{
		UDirectionalLightComponent* Sun = Cast<UDirectionalLightComponent>(It->GetLightComponent());
		Sun->SetMobility(EComponentMobility::Movable);
		Sun->SetWorldRotation(FRotator(-32.0f, 35.0f, 0.0f));
		Sun->SetLightColor(FLinearColor(1.0f, 0.96f, 0.9f));
		Sun->SetCastShadows(false);
	}

	// Clear air: only a faint low haze; the sky atmosphere's aerial perspective handles real distance.
	for (TActorIterator<AExponentialHeightFog> It(GetWorld()); It; ++It)
	{
		UExponentialHeightFogComponent* Fog = It->GetComponent();
		Fog->SetFogDensity(0.0025f);
		Fog->SetFogHeightFalloff(0.2f);
		Fog->SetStartDistance(80000.0f);
		Fog->SetFogInscatteringColor(FLinearColor(0.45f, 0.55f, 0.7f));
		Fog->SetDirectionalInscatteringExponent(8.0f);
		Fog->SetDirectionalInscatteringColor(FLinearColor(0.35f, 0.3f, 0.25f));
	}

	for (TActorIterator<ASkyLight> It(GetWorld()); It; ++It)
	{
		It->GetLightComponent()->RecaptureSky();
	}

	// Camera-like post: restrained bloom and vignette, no stylised colour shifts.
	APostProcessVolume* Post = GetWorld()->SpawnActor<APostProcessVolume>();
	Post->bUnbound = true;
	FPostProcessSettings& S = Post->Settings;
	S.bOverride_BloomIntensity = true;
	S.BloomIntensity = 0.5f;
	S.bOverride_VignetteIntensity = true;
	S.VignetteIntensity = 0.25f;
	// Hold exposure down so bright ground and sky aren't washed out.
	S.bOverride_AutoExposureBias = true;
	S.AutoExposureBias = -0.8f;
	S.bOverride_ColorContrast = true;
	S.ColorContrast = FVector4(1.0f, 1.0f, 1.0f, 1.08f);
	S.bOverride_MotionBlurAmount = true;
	S.MotionBlurAmount = 0.0f;
}

APawn* AExplorerGameMode::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
	FindOrSpawnTerrain();

	// -BiomeReport logs where each biome can be found, for testing and exploring.
	if (FParse::Param(FCommandLine::Get(), TEXT("BiomeReport")))
	{
		TMap<EBiome, FVector2D> Found;
		for (int32 Ring = 0; Ring < 60 && Found.Num() < 10; ++Ring)
		{
			const int32 Steps = FMath::Max(1, Ring * 8);
			for (int32 i = 0; i < Steps; ++i)
			{
				const float Angle = 2.0f * PI * i / Steps;
				const FVector2D P(FMath::Cos(Angle) * Ring * 500000.0f, FMath::Sin(Angle) * Ring * 500000.0f);
				const EBiome Biome = WorldGen::Sample(P.X, P.Y).Biome;
				if (!Found.Contains(Biome))
				{
					Found.Add(Biome, P);
					UE_LOG(LogTemp, Display, TEXT("BiomeReport: biome %d nearest at X=%.0f Y=%.0f (%.1f km)"), static_cast<int32>(Biome), P.X, P.Y, P.Size() / 100000.0f);
				}
			}
		}

		// Nearest landmark of each kind, searching region cells outwards from the origin.
		auto Report = [](const TCHAR* Kind, double CellSize, TFunctionRef<bool(int32, int32, FVector&)> Find)
		{
			FVector Best;
			double BestDist = TNumericLimits<double>::Max();
			for (int32 CY = -12; CY <= 12; ++CY)
			{
				for (int32 CX = -12; CX <= 12; ++CX)
				{
					FVector P;
					if (FVector2D(CX, CY).Size() * CellSize < BestDist && Find(CX, CY, P) && FVector2D(P).Size() < BestDist)
					{
						Best = P;
						BestDist = FVector2D(P).Size();
					}
				}
			}
			if (BestDist < TNumericLimits<double>::Max())
			{
				UE_LOG(LogTemp, Display, TEXT("BiomeReport: nearest %s at X=%.0f Y=%.0f Z=%.0f (%.1f km)"), Kind, Best.X, Best.Y, Best.Z, BestDist / 100000.0);
			}
		};
		Report(TEXT("village"), ATerrainStreamer::VillageCellSize(), [](int32 X, int32 Y, FVector& P) { return ATerrainStreamer::FindVillage(X, Y, P); });
		Report(TEXT("city"), ATerrainStreamer::CityCellSize(), [](int32 X, int32 Y, FVector& P) { return ATerrainStreamer::FindCity(X, Y, P); });
		Report(TEXT("floating island"), ATerrainStreamer::IslandCellSize(), [](int32 X, int32 Y, FVector& P) { float R; return ATerrainStreamer::FindFloatingIsland(X, Y, P, R); });
		Report(TEXT("lighthouse"), ATerrainStreamer::LighthouseCellSize(), [](int32 X, int32 Y, FVector& P) { return ATerrainStreamer::FindLighthouse(X, Y, P); });
		Report(TEXT("stone circle"), ATerrainStreamer::StoneCircleCellSize(), [](int32 X, int32 Y, FVector& P) { return ATerrainStreamer::FindStoneCircle(X, Y, P); });
		Report(TEXT("ruin"), ATerrainStreamer::RuinCellSize(), [](int32 X, int32 Y, FVector& P) { return ATerrainStreamer::FindRuin(X, Y, P); });
		Report(TEXT("obelisk circle"), ATerrainStreamer::ObeliskCellSize(), [](int32 X, int32 Y, FVector& P) { return ATerrainStreamer::FindObelisks(X, Y, P); });
		Report(TEXT("world tree"), ATerrainStreamer::WorldTreeCellSize(), [](int32 X, int32 Y, FVector& P) { return ATerrainStreamer::FindWorldTree(X, Y, P); });
		Report(TEXT("sky gate"), ATerrainStreamer::SkyGateCellSize(), [](int32 X, int32 Y, FVector& P) { return ATerrainStreamer::FindSkyGate(X, Y, P); });
		Report(TEXT("wind farm"), ATerrainStreamer::WindFarmCellSize(), [](int32 X, int32 Y, FVector& P) { return ATerrainStreamer::FindWindFarm(X, Y, P); });
		if (RealPlace::Load())
		{
			const FVector2D O = RealPlace::Origin();
			UE_LOG(LogTemp, Display, TEXT("BiomeReport: Council Bluffs (Bayliss Park) at X=%.0f Y=%.0f (%.1f km)"), O.X, O.Y, O.Size() / 100000.0);
		}
		Report(TEXT("volcano"), WorldGen::VolcanoCellSize(), [](int32 X, int32 Y, FVector& P)
		{
			FVector2D C; float R, H;
			if (!WorldGen::FindVolcano(X, Y, C, R, H)) return false;
			P = FVector(C, WorldGen::Height(C.X, C.Y));
			return true;
		});
	}

	// -StartAt=<name> starts over a mapped Council Bluffs landmark (e.g. -StartAt="Big Lake Park"); -CouncilBluffs starts over Bayliss Park.
	FString StartAt;
	if (FParse::Value(FCommandLine::Get(), TEXT("StartAt="), StartAt) || FParse::Param(FCommandLine::Get(), TEXT("CouncilBluffs")))
	{
		if (StartAt.IsEmpty())
		{
			StartAt = TEXT("Bayliss Park");
		}
		FVector2D At;
		if (RealPlace::Load() && RealPlace::FindLandmark(StartAt, At))
		{
			FVector Spot(At.X, At.Y, WorldGen::Height(At.X, At.Y) + 6000.0f);
			float Alt = 6000.0f;
			if (FParse::Value(FCommandLine::Get(), TEXT("StartAlt="), Alt))
			{
				Spot.Z = WorldGen::Height(At.X, At.Y) + Alt;
			}
			FParse::Value(FCommandLine::Get(), TEXT("StartZ="), Spot.Z);
			FRotator Look(-15.0f, 0.0f, 0.0f);
			FParse::Value(FCommandLine::Get(), TEXT("StartYaw="), Look.Yaw);
			FParse::Value(FCommandLine::Get(), TEXT("StartPitch="), Look.Pitch);
			// -StartDist=<cm> backs the camera up along its view direction; -StartSide=<cm> slides it sideways.
			float Dist = 0.0f, Side = 0.0f;
			FParse::Value(FCommandLine::Get(), TEXT("StartDist="), Dist);
			FParse::Value(FCommandLine::Get(), TEXT("StartSide="), Side);
			Spot -= Look.Vector() * Dist;
			Spot += FRotator(0.0f, Look.Yaw + 90.0f, 0.0f).Vector() * Side;
			return SpawnDefaultPawnAtTransform(NewPlayer, FTransform(Look, Spot));
		}
	}

	// -StartX= / -StartY= pick the starting point (world units); otherwise start over land near the origin.
	FVector Location = FVector::ZeroVector;
	// -StartZ= sets an absolute height, -StartYaw= / -StartPitch= the initial heading.
	if (FParse::Value(FCommandLine::Get(), TEXT("StartX="), Location.X) | FParse::Value(FCommandLine::Get(), TEXT("StartY="), Location.Y))
	{
		Location.Z = FMath::Max(WorldGen::Height(Location.X, Location.Y), 0.0f) + StartAltitude;
		FParse::Value(FCommandLine::Get(), TEXT("StartZ="), Location.Z);
		FRotator Rotation(-5.0f, 0.0f, 0.0f);
		FParse::Value(FCommandLine::Get(), TEXT("StartYaw="), Rotation.Yaw);
		FParse::Value(FCommandLine::Get(), TEXT("StartPitch="), Rotation.Pitch);
		return SpawnDefaultPawnAtTransform(NewPlayer, FTransform(Rotation, Location));
	}

	// Continue where we left off, unless -NewGame asks for a fresh start.
	if (!FParse::Param(FCommandLine::Get(), TEXT("NewGame")))
	{
		if (const UExplorerSaveGame* Save = UExplorerSaveGame::Load())
		{
			return SpawnDefaultPawnAtTransform(NewPlayer, FTransform(FRotator(Save->Pitch, Save->Yaw, 0.0f), Save->Location));
		}
	}
	for (int32 Ring = 0; Ring < 40; ++Ring)
	{
		bool bFound = false;
		const int32 Steps = FMath::Max(1, Ring * 6);
		for (int32 i = 0; i < Steps && !bFound; ++i)
		{
			const float Angle = 2.0f * PI * i / Steps;
			const FVector Candidate(FMath::Cos(Angle) * Ring * 300000.0f, FMath::Sin(Angle) * Ring * 300000.0f, 0.0);
			const FWorldSample S = WorldGen::Sample(Candidate.X, Candidate.Y);
			if (S.Land > 0.9f && S.Mountains < 0.2f && S.Height > 500.0f)
			{
				Location = Candidate;
				bFound = true;
			}
		}
		if (bFound)
		{
			break;
		}
	}
	Location.Z = FMath::Max(WorldGen::Height(Location.X, Location.Y), 0.0f) + StartAltitude;

	return SpawnDefaultPawnAtTransform(NewPlayer, FTransform(FRotator(-5.0f, 0.0f, 0.0f), Location));
}

ATerrainStreamer* AExplorerGameMode::FindOrSpawnTerrain()
{
	for (TActorIterator<ATerrainStreamer> It(GetWorld()); It; ++It)
	{
		return *It;
	}

	if (!TerrainClass)
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return GetWorld()->SpawnActor<ATerrainStreamer>(TerrainClass, FTransform::Identity, Params);
}
