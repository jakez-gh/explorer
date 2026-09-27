#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "InputActionValue.h"
#include "FlightPawn.generated.h"

class USpringArmComponent;
class UCameraComponent;
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
	class UPawnMovementComponent* MovementComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Flight")
	UCameraComponent* CameraComponent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	class UInputMappingContext* DefaultMappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	UInputAction* MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	UInputAction* LookAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Input")
	UInputAction* AscendAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Movement")
	float MaxFlightSpeed = 2000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flight|Movement")
	float Acceleration = 5000.0f;

	virtual void BeginPlay() override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	virtual void Tick(float DeltaTime) override;

private:
	void Move(const FInputActionValue& Value);
	void Look(const FInputActionValue& Value);
	void Ascend(const FInputActionValue& Value);

	FVector CurrentVelocity = FVector::ZeroVector;
};
