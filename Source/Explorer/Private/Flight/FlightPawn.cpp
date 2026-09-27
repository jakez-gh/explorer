#include "Flight/FlightPawn.h"
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
	bUseControllerRotationPitch = true;
	bUseControllerRotationYaw = true;
	bUseControllerRotationRoll = true;

	CollisionComponent = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	CollisionComponent->InitSphereRadius(50.0f);
	CollisionComponent->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	RootComponent = CollisionComponent;

	CameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	CameraComponent->SetupAttachment(RootComponent);
	CameraComponent->bUsePawnControlRotation = true;
}

void AFlightPawn::BeginPlay()
{
	Super::BeginPlay();

	Speed = CruiseSpeed;
	Velocity = GetActorForwardVector() * Speed;
	LastYaw = GetControlRotation().Yaw;
	CameraComponent->SetFieldOfView(BaseFieldOfView);
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

void AFlightPawn::CreateDefaultInput()
{
	if (!MoveAction)
	{
		MoveAction = MakeAction(this, TEXT("IA_Move"), EInputActionValueType::Axis2D);
	}
	if (!LookAction)
	{
		LookAction = MakeAction(this, TEXT("IA_Look"), EInputActionValueType::Axis2D);
	}
	if (!AscendAction)
	{
		AscendAction = MakeAction(this, TEXT("IA_Ascend"), EInputActionValueType::Axis1D);
	}
	if (!BoostAction)
	{
		BoostAction = MakeAction(this, TEXT("IA_Boost"), EInputActionValueType::Boolean);
	}
	if (DefaultMappingContext)
	{
		return;
	}

	UInputMappingContext* Context = NewObject<UInputMappingContext>(this, TEXT("IMC_Flight"));

	// Move: X = turn (banks), Y = throttle.
	AddModifier<UInputModifierDeadZone>(Context, Context->MapKey(MoveAction, EKeys::Gamepad_Left2D));
	MapKeyToAxis(Context, MoveAction, EKeys::W, true, false);
	MapKeyToAxis(Context, MoveAction, EKeys::S, true, true);
	MapKeyToAxis(Context, MoveAction, EKeys::D, false, false);
	MapKeyToAxis(Context, MoveAction, EKeys::A, false, true);

	// Look: X = yaw, Y = pitch (positive = nose up). Gamepad is a rate, mouse is a delta.
	{
		FEnhancedActionKeyMapping& Mapping = Context->MapKey(LookAction, EKeys::Gamepad_Right2D);
		AddModifier<UInputModifierDeadZone>(Context, Mapping);
		AddModifier<UInputModifierScaleByDeltaTime>(Context, Mapping);
		AddModifier<UInputModifierScalar>(Context, Mapping)->Scalar = FVector(GamepadLookRate);
	}
	{
		FEnhancedActionKeyMapping& Mapping = Context->MapKey(LookAction, EKeys::Mouse2D);
		AddModifier<UInputModifierScalar>(Context, Mapping)->Scalar = FVector(MouseSensitivity);
	}

	// Ascend: triggers on gamepad, E/Q or Space/Ctrl on keyboard.
	Context->MapKey(AscendAction, EKeys::Gamepad_RightTriggerAxis);
	AddModifier<UInputModifierNegate>(Context, Context->MapKey(AscendAction, EKeys::Gamepad_LeftTriggerAxis));
	Context->MapKey(AscendAction, EKeys::E);
	AddModifier<UInputModifierNegate>(Context, Context->MapKey(AscendAction, EKeys::Q));
	Context->MapKey(AscendAction, EKeys::SpaceBar);
	AddModifier<UInputModifierNegate>(Context, Context->MapKey(AscendAction, EKeys::LeftControl));

	// Boost: Shift, or a bumper / left stick click.
	Context->MapKey(BoostAction, EKeys::LeftShift);
	Context->MapKey(BoostAction, EKeys::Gamepad_LeftShoulder);
	Context->MapKey(BoostAction, EKeys::Gamepad_RightShoulder);
	Context->MapKey(BoostAction, EKeys::Gamepad_LeftThumbstick);

	DefaultMappingContext = Context;
}

void AFlightPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	CreateDefaultInput();

	if (APlayerController* PlayerController = GetController<APlayerController>())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(DefaultMappingContext, 0);
		}
	}

	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		// Continuous inputs are polled each tick so releasing them reliably reads as zero.
		EnhancedInputComponent->BindActionValue(MoveAction);
		EnhancedInputComponent->BindActionValue(AscendAction);
		EnhancedInputComponent->BindActionValue(BoostAction);
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AFlightPawn::Look);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("FlightPawn requires EnhancedInputComponent; check DefaultInput.ini"));
	}
}

void AFlightPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (DeltaTime <= 0.0f)
	{
		return;
	}

	FVector2D Move = FVector2D::ZeroVector;
	float Ascend = 0.0f;
	bool bBoost = false;
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(InputComponent))
	{
		Move = EnhancedInputComponent->GetBoundActionValue(MoveAction).Get<FVector2D>();
		Ascend = FMath::Clamp(EnhancedInputComponent->GetBoundActionValue(AscendAction).Get<float>(), -1.0f, 1.0f);
		bBoost = EnhancedInputComponent->GetBoundActionValue(BoostAction).Get<bool>();
	}

	// Turning input yaws; the bank follows from how fast we're actually turning, whatever the source.
	AddControllerYawInput(Move.X * TurnRate * DeltaTime);

	const FRotator Control = GetControlRotation();
	const float MeasuredYawRate = FMath::FindDeltaAngleDegrees(LastYaw, Control.Yaw) / DeltaTime;
	LastYaw = Control.Yaw;
	YawRate = FMath::FInterpTo(YawRate, MeasuredYawRate, DeltaTime, 6.0f);
	// Roll into the turn: turning left (negative yaw rate) lowers the left wing, which is positive roll here.
	const float TargetBank = -FMath::Clamp(YawRate / TurnRate, -1.0f, 1.0f) * MaxBankAngle;
	Bank = FMath::FInterpTo(Bank, TargetBank, DeltaTime, BankResponse);
	if (Controller)
	{
		Controller->SetControlRotation(FRotator(Control.Pitch, Control.Yaw, Bank));
	}

	// Throttle eases speed between hovering, cruise and full speed; boost multiplies it.
	float TargetSpeed = CruiseSpeed + FMath::Max(Move.Y, 0.0f) * (MaxFlightSpeed - CruiseSpeed) + FMath::Min(Move.Y, 0.0f) * CruiseSpeed;
	if (bBoost)
	{
		TargetSpeed = FMath::Max(TargetSpeed, CruiseSpeed) * BoostMultiplier;
	}
	Speed = FMath::FInterpTo(Speed, TargetSpeed, DeltaTime, SpeedResponse);

	// Glide: the direction of travel drifts towards where we're looking.
	const FVector Forward = FRotator(Control.Pitch, Control.Yaw, 0.0f).Vector();
	const FVector Desired = Forward * Speed + FVector::UpVector * Ascend * AscendSpeed;
	Velocity = FMath::VInterpTo(Velocity, Desired, DeltaTime, GlideResponse);

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
		const FVector Remaining = FVector::VectorPlaneProject(Delta * (1.0f - Hit.Time), Hit.Normal);
		Velocity = FVector::VectorPlaneProject(Velocity, Hit.Normal);
		AddActorWorldOffset(Remaining, true);
	}
	if (GetActorLocation().Z < Ground + 300.0f)
	{
		SetActorLocation(FVector(GetActorLocation().X, GetActorLocation().Y, Ground + 300.0f));
	}

	static const bool bDebug = FParse::Param(FCommandLine::Get(), TEXT("FlightDebug"));
	if (bDebug && FMath::FloorToInt(BobTime) != FMath::FloorToInt(BobTime + DeltaTime))
	{
		UE_LOG(LogTemp, Display, TEXT("FlightDebug: fps=%.0f z=%.0f ground=%.0f ahead=%.0f alt=%.0f vel=%s speed=%.0f hit=%d pitch=%.1f"),
			1.0f / DeltaTime, GetActorLocation().Z, Ground, GroundAhead, GetActorLocation().Z - Ground, *Velocity.ToCompactString(), Speed, Hit.bBlockingHit ? 1 : 0, Control.Pitch);
	}

	// Widen the view with speed, and let the camera breathe.
	const float SpeedAlpha = FMath::Clamp((Speed - CruiseSpeed) / (MaxFlightSpeed * BoostMultiplier - CruiseSpeed), 0.0f, 1.0f);
	CameraComponent->SetFieldOfView(FMath::FInterpTo(CameraComponent->FieldOfView, BaseFieldOfView + SpeedFieldOfView * SpeedAlpha, DeltaTime, 2.0f));
	BobTime += DeltaTime;
	CameraComponent->SetRelativeLocation(FVector(0.0f, 0.0f, FMath::Sin(BobTime * 2.0f * PI / BobPeriod) * BobAmplitude));
}

void AFlightPawn::Look(const FInputActionValue& Value)
{
	const FVector2D LookAxisVector = Value.Get<FVector2D>();
	AddControllerYawInput(LookAxisVector.X);
	AddControllerPitchInput(bInvertPitch ? -LookAxisVector.Y : LookAxisVector.Y);
}
