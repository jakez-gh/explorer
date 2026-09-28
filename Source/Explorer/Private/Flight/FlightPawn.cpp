#include "Flight/FlightPawn.h"
#include "Game/ExplorerSaveGame.h"
#include "Procedural/WorldGen.h"
#include "Camera/CameraComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/LocalPlayer.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"

AFlightPawn::AFlightPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	CollisionComponent = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	CollisionComponent->InitSphereRadius(50.0f);
	CollisionComponent->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	RootComponent = CollisionComponent;

	// The camera is aimed explicitly each tick from heading, bank and look.
	CameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	CameraComponent->SetupAttachment(RootComponent);
	CameraComponent->bUsePawnControlRotation = false;
}

void AFlightPawn::BeginPlay()
{
	Super::BeginPlay();

	FlightYaw = GetActorRotation().Yaw;
	FlightPitch = 0.0f;
	SetActorRotation(FRotator(0.0f, FlightYaw, 0.0f));
	const float Ground = FMath::Max(WorldGen::Height(GetActorLocation().X, GetActorLocation().Y), 0.0f);
	TargetAltitude = FMath::Clamp(GetActorLocation().Z - Ground, MinGroundClearance, MaxAltitude);
	CameraComponent->SetFieldOfView(FieldOfView);
}

void AFlightPawn::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	SaveProgress();
	Super::EndPlay(EndPlayReason);
}

void AFlightPawn::SaveProgress() const
{
	UExplorerSaveGame::Save(GetActorLocation(), FlightYaw, FlightPitch);
}

namespace
{
	UInputAction* MakeAction(UObject* Outer, const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* Action = NewObject<UInputAction>(Outer, Name);
		Action->ValueType = Type;
		return Action;
	}

	template <typename TModifier>
	TModifier* AddModifier(UInputMappingContext* Context, FEnhancedActionKeyMapping& Mapping)
	{
		TModifier* Modifier = NewObject<TModifier>(Context);
		Mapping.Modifiers.Add(Modifier);
		return Modifier;
	}

	// Maps a digital key onto one component of a 2D axis action.
	void MapKeyToAxis(UInputMappingContext* Context, UInputAction* Action, FKey Key, bool bYAxis, bool bNegate)
	{
		FEnhancedActionKeyMapping& Mapping = Context->MapKey(Action, Key);
		if (bYAxis)
		{
			AddModifier<UInputModifierSwizzleAxis>(Context, Mapping)->Order = EInputAxisSwizzle::YXZ;
		}
		if (bNegate)
		{
			AddModifier<UInputModifierNegate>(Context, Mapping);
		}
	}
}

void AFlightPawn::CreateInput()
{
	if (MappingContext)
	{
		return;
	}

	SteerAction = MakeAction(this, TEXT("IA_Steer"), EInputActionValueType::Axis2D);
	LookAction = MakeAction(this, TEXT("IA_Look"), EInputActionValueType::Axis2D);
	MouseLookAction = MakeAction(this, TEXT("IA_MouseLook"), EInputActionValueType::Axis2D);
	ThrottleAction = MakeAction(this, TEXT("IA_Throttle"), EInputActionValueType::Axis1D);
	RiseAction = MakeAction(this, TEXT("IA_Rise"), EInputActionValueType::Axis1D);

	MappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_Flight"));

	// Gamepad (primary).
	AddModifier<UInputModifierDeadZone>(MappingContext, MappingContext->MapKey(SteerAction, EKeys::Gamepad_Left2D));
	AddModifier<UInputModifierDeadZone>(MappingContext, MappingContext->MapKey(LookAction, EKeys::Gamepad_Right2D));
	MappingContext->MapKey(ThrottleAction, EKeys::Gamepad_RightTriggerAxis);
	MappingContext->MapKey(RiseAction, EKeys::Gamepad_RightShoulder);
	AddModifier<UInputModifierNegate>(MappingContext, MappingContext->MapKey(RiseAction, EKeys::Gamepad_LeftShoulder));

	// Keyboard and mouse (fallback): WASD steer, Space cruise, Shift full speed, E/Q rise/sink, mouse look.
	MapKeyToAxis(MappingContext, SteerAction, EKeys::D, false, false);
	MapKeyToAxis(MappingContext, SteerAction, EKeys::A, false, true);
	MapKeyToAxis(MappingContext, SteerAction, EKeys::W, true, false);
	MapKeyToAxis(MappingContext, SteerAction, EKeys::S, true, true);
	AddModifier<UInputModifierScalar>(MappingContext, MappingContext->MapKey(ThrottleAction, EKeys::SpaceBar))->Scalar = FVector(0.45f);
	MappingContext->MapKey(ThrottleAction, EKeys::LeftShift);
	MappingContext->MapKey(RiseAction, EKeys::E);
	AddModifier<UInputModifierNegate>(MappingContext, MappingContext->MapKey(RiseAction, EKeys::Q));
	AddModifier<UInputModifierScalar>(MappingContext, MappingContext->MapKey(MouseLookAction, EKeys::Mouse2D))->Scalar = FVector(MouseSensitivity);
}

void AFlightPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	CreateInput();

	if (APlayerController* PlayerController = GetController<APlayerController>())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(MappingContext, 0);
		}
	}

	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		// Continuous inputs are polled each tick so releasing them reliably reads as zero.
		EnhancedInputComponent->BindActionValue(SteerAction);
		EnhancedInputComponent->BindActionValue(LookAction);
		EnhancedInputComponent->BindActionValue(ThrottleAction);
		EnhancedInputComponent->BindActionValue(RiseAction);
		EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &AFlightPawn::OnMouseLook);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("FlightPawn requires EnhancedInputComponent; check DefaultInput.ini"));
	}
}

void AFlightPawn::OnMouseLook(const FInputActionValue& Value)
{
	const FVector2D Delta = Value.Get<FVector2D>();
	MouseLook.X = FMath::Clamp(MouseLook.X + Delta.X, -LookYawRange, LookYawRange);
	MouseLook.Y = FMath::Clamp(MouseLook.Y + Delta.Y, -LookPitchRange, LookPitchRange);
	MouseIdleTime = 0.0f;
}

void AFlightPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (DeltaTime <= 0.0f)
	{
		return;
	}

	FVector2D Steer = FVector2D::ZeroVector;
	FVector2D Stick = FVector2D::ZeroVector;
	float Throttle = 0.0f;
	float Rise = 0.0f;
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(InputComponent))
	{
		Steer = EnhancedInputComponent->GetBoundActionValue(SteerAction).Get<FVector2D>();
		Stick = EnhancedInputComponent->GetBoundActionValue(LookAction).Get<FVector2D>();
		Throttle = FMath::Clamp(EnhancedInputComponent->GetBoundActionValue(ThrottleAction).Get<float>(), 0.0f, 1.0f);
		Rise = FMath::Clamp(EnhancedInputComponent->GetBoundActionValue(RiseAction).Get<float>(), -1.0f, 1.0f);
	}
	Steer.X = FMath::Clamp(Steer.X, -1.0f, 1.0f);
	Steer.Y = FMath::Clamp(bInvertPitch ? -Steer.Y : Steer.Y, -1.0f, 1.0f);

	// Heading: the stick turns; bank into the turn in proportion to the stick, level out when released.
	FlightYaw = FRotator::NormalizeAxis(FlightYaw + Steer.X * TurnRate * DeltaTime);
	Bank = FMath::FInterpTo(Bank, Steer.X * MaxBankAngle, DeltaTime, BankResponse);

	// Speed follows the trigger directly: released means stop and hover. Travel is level; height is
	// handled by the altitude hold below.
	Speed = MaxFlightSpeed * FMath::Pow(Throttle, ThrottleCurve);
	FVector Velocity = FRotator(0.0f, FlightYaw, 0.0f).Vector() * Speed;

	// Terrain-following altitude hold. The stick's up/down and the bumpers change the height held above
	// the ground (faster when already high); the pawn then accelerates up or down to keep that height
	// over the terrain ahead, so it rises over hills before reaching them and sinks into valleys.
	const FVector Location = GetActorLocation();
	const float Ground = FMath::Max(WorldGen::Height(Location.X, Location.Y), 0.0f);
	const float AltitudeInput = FMath::Clamp(Steer.Y + Rise, -1.0f, 1.0f);
	TargetAltitude = FMath::Clamp(TargetAltitude + AltitudeInput * (AltitudeChangeRate + TargetAltitude * 0.8f) * DeltaTime, MinGroundClearance, MaxAltitude);

	auto GroundAt = [&](float Seconds)
	{
		const FVector P = Location + Velocity * Seconds;
		return FMath::Max(WorldGen::Height(P.X, P.Y), 0.0f);
	};
	// Follow the ground just ahead, feeding forward its slope so hills are climbed as they arrive
	// rather than after; and never let a crest further ahead come closer than half the held height.
	const float Near = GroundAt(0.15f);
	const float SlopeVZ = Speed > 10.0f ? (GroundAt(0.5f) - Near) / 0.35f : 0.0f;
	float TargetZ = Near + TargetAltitude;
	for (const float Seconds : { 0.3f, 0.6f })
	{
		TargetZ = FMath::Max(TargetZ, GroundAt(Seconds) + MinGroundClearance);
	}
	const float MaxClimb = 1500.0f + Speed * 1.2f + TargetAltitude * 0.5f;
	const float Accel = AltitudeAccel + Speed * 1.5f;
	float DesiredVZ = FMath::Clamp(SlopeVZ + (TargetZ - Location.Z) * AltitudeStiffness, -MaxClimb, MaxClimb);
	// Never descend faster than can be stopped before reaching the lowest allowed height.
	const float Room = FMath::Max(Location.Z - (FMath::Max(Ground, Near) + MinGroundClearance), 0.0f);
	DesiredVZ = FMath::Max(DesiredVZ, -FMath::Sqrt(2.0f * Accel * 0.7f * Room));
	VerticalSpeed += FMath::Clamp(DesiredVZ - VerticalSpeed, -Accel * DeltaTime, Accel * DeltaTime);
	Velocity.Z = VerticalSpeed;

	// The nose follows the flight path a little, so climbs and descents are felt.
	const float PathPitch = FMath::RadiansToDegrees(FMath::Atan2(VerticalSpeed, FMath::Max(Speed, 1500.0f)));
	FlightPitch = FMath::FInterpTo(FlightPitch, FMath::Clamp(PathPitch * 0.6f, -25.0f, 25.0f), DeltaTime, 2.0f);

	// Move, sliding along anything solid.
	FHitResult Hit;
	const FVector Delta = Velocity * DeltaTime;
	AddActorWorldOffset(Delta, true, &Hit);
	if (Hit.bBlockingHit)
	{
		VerticalSpeed *= FMath::Max(0.0f, 1.0f - FMath::Abs(Hit.Normal.Z));
		const FVector Remaining = Delta * (1.0f - Hit.Time);
		FVector Slide = FVector::VectorPlaneProject(Remaining, Hit.Normal);
		// Head-on into a trunk or wall: veer round it rather than stopping dead.
		if (Slide.SizeSquared() < Remaining.SizeSquared() * 0.25f)
		{
			FVector Around = FVector::CrossProduct(Hit.Normal, FVector::UpVector).GetSafeNormal();
			if (FVector::DotProduct(Around, FRotator(0.0f, FlightYaw, 0.0f).RotateVector(FVector::RightVector)) < 0.0f)
			{
				Around = -Around;
			}
			Slide += Around * Remaining.Size() * 0.8f;
		}
		AddActorWorldOffset(Slide, true);
	}
	if (GetActorLocation().Z < Ground + 60.0f)
	{
		SetActorLocation(FVector(GetActorLocation().X, GetActorLocation().Y, Ground + 60.0f));
	}
	SetActorRotation(FRotator(0.0f, FlightYaw, 0.0f));

	// Look: the right stick points the view directly; the mouse drifts back to centre when left alone.
	MouseIdleTime += DeltaTime;
	if (MouseIdleTime > 0.6f)
	{
		MouseLook = FMath::Vector2DInterpTo(MouseLook, FVector2D::ZeroVector, DeltaTime, 2.5f);
	}
	FVector2D TargetLook(Stick.X * LookYawRange + MouseLook.X, Stick.Y * LookPitchRange + MouseLook.Y);
	TargetLook.X = FMath::Clamp(TargetLook.X, -LookYawRange, LookYawRange);
	// Stop at straight up and straight down rather than tumbling over.
	TargetLook.Y = FMath::Clamp(TargetLook.Y, -85.0f - FlightPitch, 85.0f - FlightPitch);
	Look = FMath::Vector2DInterpTo(Look, TargetLook, DeltaTime, LookResponse);

	const FQuat Body = FRotator(FlightPitch, FlightYaw, Bank).Quaternion();
	const FQuat Head = FRotator(Look.Y, Look.X, 0.0f).Quaternion();
	CameraComponent->SetWorldRotation(Body * Head);

	// Let the camera breathe; speed never changes the view.
	BobTime += DeltaTime;
	CameraComponent->SetRelativeLocation(FVector(0.0f, 0.0f, FMath::Sin(BobTime * 2.0f * PI / BobPeriod) * BobAmplitude));

	// Remember where we are.
	SaveTimer += DeltaTime;
	if (SaveTimer > 5.0f)
	{
		SaveTimer = 0.0f;
		SaveProgress();
	}

	static const bool bDebug = FParse::Param(FCommandLine::Get(), TEXT("FlightDebug"));
	if (bDebug && FMath::FloorToInt(BobTime) != FMath::FloorToInt(BobTime - DeltaTime))
	{
		UE_LOG(LogTemp, Display, TEXT("FlightDebug: fps=%.0f loc=%s alt=%.0f speed=%.0f yaw=%.1f pitch=%.1f bank=%.1f look=%s"),
			1.0f / DeltaTime, *GetActorLocation().ToCompactString(), GetActorLocation().Z - Ground, Speed, FlightYaw, FlightPitch, Bank, *Look.ToString());
	}
}
