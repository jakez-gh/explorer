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

	// Top speed near the ground (cm/s), rising to MaxFlightSpeed by FullSpeedAltitude above it.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Speed")
	float LowAltitudeMaxSpeed = 9000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Speed")
	float LowSpeedAltitude = 1500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Speed")
	float FullSpeedAltitude = 30000.0f;

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
	float AutoLevelRate = 1.2f;

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

	// Lowest height held above ground or water (you can skim among the trees).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Altitude")
	float MinGroundClearance = 200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Altitude")
	float MaxAltitude = 50000.0f;

	// Flying into a tree's crown slows you, up to this fraction at the trunk, within this radius (cm).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Trees")
	float CanopyDrag = 0.75f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Trees")
	float CanopyRadius = 700.0f;

	// How fast full stick changes the held height near the ground (cm/s); it scales up with height.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Altitude")
	float AltitudeChangeRate = 800.0f;

	// How firmly the held height is tracked (1/s), and the vertical acceleration available (cm/s^2).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Altitude")
	float AltitudeStiffness = 0.5f;

	// How much of the ground's rise and fall is followed when flying level (0 = none, 1 = all).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Altitude")
	float TerrainFollow = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Altitude")
	float AltitudeAccel = 1500.0f;

	// Caps on how hard level flight is drawn up or down (cm/s): towards the cruising height, and along
	// the ground's slope.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Altitude")
	float MaxCorrectionSpeed = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Altitude")
	float MaxFollowSpeed = 1200.0f;

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

	// 0 in the open, 1 pressed against a trunk; smoothed.
	float Foliage = 0.0f;
	float SpeedCap = 0.0f;
	TWeakObjectPtr<class ATerrainStreamer> Streamer;
	float TargetAltitude = 1000.0f;
	float VerticalSpeed = 0.0f;
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
