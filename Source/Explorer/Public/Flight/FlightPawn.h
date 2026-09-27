#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "InputActionValue.h"
#include "FlightPawn.generated.h"

class UCameraComponent;
class USphereComponent;
class UFloatingPawnMovement;
class UInputMappingContext;
class UInputAction;

UCLASS()
class EXPLORER_API AFlightPawn : public APawn
{
	GENERATED_BODY()

public:
	AFlightPawn();

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flight")
	USphereComponent* CollisionComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flight")
	UFloatingPawnMovement* MovementComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flight")
	UCameraComponent* CameraComponent;

	// Input assets are optional: any left unset are created in code with default
	// gamepad + keyboard/mouse bindings.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	UInputMappingContext* DefaultMappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	UInputAction* MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	UInputAction* LookAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	UInputAction* AscendAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Movement")
	float MaxFlightSpeed = 4000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Movement")
	float Acceleration = 4000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Movement")
	float Deceleration = 2000.0f;

	// Degrees per second at full stick deflection.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	float GamepadLookRate = 90.0f;

	// Degrees per mouse count.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	float MouseSensitivity = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	bool bInvertPitch = false;

	virtual void BeginPlay() override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	virtual void Tick(float DeltaTime) override;

private:
	void CreateDefaultInput();
	void Look(const FInputActionValue& Value);

	FVector CurrentVelocity = FVector::ZeroVector;
};
