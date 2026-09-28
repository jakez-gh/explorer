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
 * Controller-first, dreamlike flight.
 *   Left stick   steer: X turns (and banks into the turn), Y pitches the nose.
 *   Right trigger throttle: speed follows the pull directly; released = hover in place.
 *   Right stick  look: the view points where the stick points, and returns forward when released.
 *   Bumpers      rise / sink.
 * The ground gently pushes you away instead of stopping you. Progress is saved and restored.
 */
UCLASS()
class EXPLORER_API AFlightPawn : public APawn
{
	GENERATED_BODY()

public:
	AFlightPawn();

	float GetFlightYaw() const { return FlightYaw; }
	float GetFlightPitch() const { return FlightPitch; }

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flight")
	USphereComponent* CollisionComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flight")
	UCameraComponent* CameraComponent;

	// Speed at full trigger.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Speed")
	float MaxFlightSpeed = 40000.0f;

	// >1 gives finer control at low trigger pull.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Speed")
	float ThrottleCurve = 2.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Speed")
	float RiseSpeed = 2500.0f;

	// Degrees per second at full stick.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Steering")
	float TurnRate = 60.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Steering")
	float PitchRate = 45.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Steering")
	float MaxPitch = 75.0f;

	// How quickly the nose drifts back to level when not pitching (per second).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Steering")
	float AutoLevelRate = 0.4f;

	// Flight-stick convention: push forward to dip the nose, pull back to climb.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Steering")
	bool bInvertPitch = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Steering")
	float MaxBankAngle = 35.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Steering")
	float BankResponse = 3.0f;

	// Look angle at full right-stick deflection.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Look")
	float LookYawRange = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Look")
	float LookPitchRange = 70.0f;

	// How tightly the view follows the stick (per second).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Look")
	float LookResponse = 12.0f;

	// Degrees per mouse count (keyboard/mouse fallback).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Look")
	float MouseSensitivity = 0.15f;

	// Closest you can get to the ground or water when flying slowly (you can skim among the trees).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Ground")
	float MinGroundClearance = 150.0f;

	// Extra clearance per unit of speed, so fast flight stays safely above the terrain (seconds of warning).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Ground")
	float ClearancePerSpeed = 0.15f;

	// How gently a dive is levelled out as the ground approaches (seconds to close the remaining gap).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Ground")
	float GroundEaseTime = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Feel")
	float FieldOfView = 90.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Feel")
	float BobAmplitude = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Feel")
	float BobPeriod = 7.0f;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	virtual void Tick(float DeltaTime) override;

private:
	void CreateInput();
	void OnMouseLook(const FInputActionValue& Value);
	void SaveProgress() const;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MappingContext;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> SteerAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MouseLookAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ThrottleAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> RiseAction;

	float FlightYaw = 0.0f;
	float FlightPitch = 0.0f;
	float Bank = 0.0f;
	FVector2D Look = FVector2D::ZeroVector;
	FVector2D MouseLook = FVector2D::ZeroVector;
	float MouseIdleTime = 0.0f;
	float Speed = 0.0f;
	float BobTime = 0.0f;
	float SaveTimer = 0.0f;
};
