#include "Flight/FlightPawn.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "InputSystem.h"

AFlightPawn::AFlightPawn()
{
	PrimaryActorTick.TickInterval = 0.0f;
	bUseControllerRotationPitch = true;
	bUseControllerRotationYaw = true;
	bUseControllerRotationRoll = true;

	CameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	CameraComponent->SetupAttachment(RootComponent);
	CameraComponent->bUsePawnControlRotation = true;

	MovementComponent = CreateDefaultSubobject<UFloatingPawnMovement>(TEXT("Movement"));
	MovementComponent->MaxSpeed = MaxFlightSpeed;
	MovementComponent->Acceleration = Acceleration;
}

void AFlightPawn::BeginPlay()
{
	Super::BeginPlay();

	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			PlayerController->GetLocalPlayer()->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			Subsystem->AddMappingContext(DefaultMappingContext, 0);
		}
	}
}

void AFlightPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AFlightPawn::Move);
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AFlightPawn::Look);
		EnhancedInputComponent->BindAction(AscendAction, ETriggerEvent::Triggered, this, &AFlightPawn::Ascend);
	}
}

void AFlightPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (MovementComponent)
	{
		AddMovementInput(GetActorForwardVector(), CurrentVelocity.X);
		AddMovementInput(GetActorRightVector(), CurrentVelocity.Y);
		AddMovementInput(GetActorUpVector(), CurrentVelocity.Z);
	}
}

void AFlightPawn::Move(const FInputActionValue& Value)
{
	const FVector2D MovementVector = Value.Get<FVector2D>();
	CurrentVelocity.X = MovementVector.Y;
	CurrentVelocity.Y = MovementVector.X;
}

void AFlightPawn::Look(const FInputActionValue& Value)
{
	const FVector2D LookAxisVector = Value.Get<FVector2D>();
	AddControllerYawInput(LookAxisVector.X);
	AddControllerPitchInput(LookAxisVector.Y);
}

void AFlightPawn::Ascend(const FInputActionValue& Value)
{
	const float AscendAxisValue = Value.Get<float>();
	CurrentVelocity.Z = AscendAxisValue;
}
