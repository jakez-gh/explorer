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
	FlightPitch = FMath::Clamp(GetActorRotation().Pitch, -MaxPitch, MaxPitch);
	SetActorRotation(FRotator(0.0f, FlightYaw, 0.0f));
	CameraComponent->SetFieldOfView(BaseFieldOfView);
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

	// Heading: turn and pitch at a rate set by the stick; the nose drifts back to level when left alone.
	FlightYaw = FRotator::NormalizeAxis(FlightYaw + Steer.X * TurnRate * DeltaTime);
	FlightPitch = FMath::Clamp(FlightPitch + Steer.Y * PitchRate * DeltaTime, -MaxPitch, MaxPitch);
	if (FMath::Abs(Steer.Y) < 0.05f)
	{
		FlightPitch = FMath::FInterpTo(FlightPitch, 0.0f, DeltaTime, AutoLevelRate);
	}

	// Bank into the turn in proportion to the stick; level out when it's released.
	Bank = FMath::FInterpTo(Bank, Steer.X * MaxBankAngle, DeltaTime, BankResponse);

	// Speed follows the trigger directly: released means stop and hover.
	Speed = MaxFlightSpeed * FMath::Pow(Throttle, ThrottleCurve);
	const FVector Forward = FRotator(FlightPitch, FlightYaw, 0.0f).Vector();
	FVector Velocity = Forward * Speed + FVector::UpVector * Rise * RiseSpeed;

	// Soft cushion above land and water, looking ahead so rising hills lift you before you reach them.
	const FVector Location = GetActorLocation();
	const float Ground = FMath::Max(WorldGen::Height(Location.X, Location.Y), 0.0f);
	float GroundAhead = Ground;
	const FVector Horizontal(Velocity.X, Velocity.Y, 0.0f);
	for (const float Seconds : { 0.5f, 1.0f, 2.0f })
	{
		const FVector Ahead = Location + Horizontal * Seconds;
		GroundAhead = FMath::Max(GroundAhead, FMath::Max(WorldGen::Height(Ahead.X, Ahead.Y), 0.0f) - 500.0f * Seconds);
	}
	const float Altitude = Location.Z - GroundAhead;
	if (Altitude < GroundCushion)
	{
		const float Push = FMath::Square(1.0f - FMath::Max(Altitude, 0.0f) / GroundCushion);
		Velocity.Z = FMath::Max(Velocity.Z, Push * 9000.0f);
	}

	// Move, sliding along anything solid.
	FHitResult Hit;
	const FVector Delta = Velocity * DeltaTime;
	AddActorWorldOffset(Delta, true, &Hit);
	if (Hit.bBlockingHit)
	{
		AddActorWorldOffset(FVector::VectorPlaneProject(Delta * (1.0f - Hit.Time), Hit.Normal), true);
	}
	if (GetActorLocation().Z < Ground + 300.0f)
	{
		SetActorLocation(FVector(GetActorLocation().X, GetActorLocation().Y, Ground + 300.0f));
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

	// Widen the view with speed, and let the camera breathe.
	const float SpeedAlpha = Speed / MaxFlightSpeed;
	CameraComponent->SetFieldOfView(FMath::FInterpTo(CameraComponent->FieldOfView, BaseFieldOfView + SpeedFieldOfView * SpeedAlpha, DeltaTime, 3.0f));
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
