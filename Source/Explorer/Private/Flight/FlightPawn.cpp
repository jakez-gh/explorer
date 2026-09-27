#include "Flight/FlightPawn.h"
#include "Camera/CameraComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/FloatingPawnMovement.h"
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

	MovementComponent = CreateDefaultSubobject<UFloatingPawnMovement>(TEXT("Movement"));
	MovementComponent->UpdatedComponent = CollisionComponent;
}

void AFlightPawn::BeginPlay()
{
	Super::BeginPlay();

	MovementComponent->MaxSpeed = MaxFlightSpeed;
	MovementComponent->Acceleration = Acceleration;
	MovementComponent->Deceleration = Deceleration;
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
	if (DefaultMappingContext)
	{
		return;
	}

	UInputMappingContext* Context = NewObject<UInputMappingContext>(this, TEXT("IMC_Flight"));

	// Move: X = strafe right, Y = forward.
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
		// Move and ascend are polled each tick so releasing input reliably reads as zero.
		EnhancedInputComponent->BindActionValue(MoveAction);
		EnhancedInputComponent->BindActionValue(AscendAction);
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

	FVector DesiredVelocity = FVector::ZeroVector;
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(InputComponent))
	{
		const FVector2D Move = EnhancedInputComponent->GetBoundActionValue(MoveAction).Get<FVector2D>();
		DesiredVelocity.X = Move.Y;
		DesiredVelocity.Y = Move.X;
		DesiredVelocity.Z = FMath::Clamp(EnhancedInputComponent->GetBoundActionValue(AscendAction).Get<float>(), -1.0f, 1.0f);
	}

	CurrentVelocity = FMath::VInterpTo(CurrentVelocity, DesiredVelocity, DeltaTime, 4.0f);

	AddMovementInput(GetActorForwardVector(), CurrentVelocity.X);
	AddMovementInput(GetActorRightVector(), CurrentVelocity.Y);
	AddMovementInput(FVector::UpVector, CurrentVelocity.Z);
}

void AFlightPawn::Look(const FInputActionValue& Value)
{
	const FVector2D LookAxisVector = Value.Get<FVector2D>();
	AddControllerYawInput(LookAxisVector.X);
	AddControllerPitchInput(bInvertPitch ? -LookAxisVector.Y : LookAxisVector.Y);
}
