#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "InputActionValue.h"
#include "FlightPawn.generated.h"

class UCameraComponent;
class USphereComponent;
class UInputMappingContext;
class UInputAction;

/**
 * Dreamlike first-person flight: you glide forward with gentle momentum, bank into turns,
 * and the ground softly pushes you away instead of stopping you.
 */
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	UInputAction* BoostAction;

	// Speed when no throttle is held.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Movement")
	float CruiseSpeed = 2500.0f;

	// Speed at full throttle, before boost.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Movement")
	float MaxFlightSpeed = 9000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Movement")
	float BoostMultiplier = 3.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Movement")
	float AscendSpeed = 3000.0f;

	// How quickly speed eases towards the throttle setting (per second).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Movement")
	float SpeedResponse = 0.9f;

	// How quickly the direction of travel follows the view (per second); lower glides more.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Movement")
	float GlideResponse = 1.6f;

	// Degrees per second at full turn input.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Turning")
	float TurnRate = 55.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Turning")
	float MaxBankAngle = 35.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Turning")
	float BankResponse = 2.5f;

	// Degrees per second at full stick deflection.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	float GamepadLookRate = 70.0f;

	// Degrees per mouse count.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	float MouseSensitivity = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	bool bInvertPitch = false;

	// Below this height above ground or water, you're gently lifted.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Feel")
	float GroundCushion = 8000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Feel")
	float BaseFieldOfView = 90.0f;

	// Extra field of view at top boosted speed.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Feel")
	float SpeedFieldOfView = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Feel")
	float BobAmplitude = 15.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Feel")
	float BobPeriod = 6.0f;

	virtual void BeginPlay() override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	virtual void Tick(float DeltaTime) override;

private:
	void CreateDefaultInput();
	void Look(const FInputActionValue& Value);

	FVector Velocity = FVector::ZeroVector;
	float Speed = 0.0f;
	float Bank = 0.0f;
	float YawRate = 0.0f;
	float LastYaw = 0.0f;
	float BobTime = 0.0f;
};
