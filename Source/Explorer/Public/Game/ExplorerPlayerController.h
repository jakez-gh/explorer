#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "ExplorerPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class SWidget;

/** Handles game-wide input: Escape toggles the pause menu, Alt+Q quits immediately. */
UCLASS()
class EXPLORER_API AExplorerPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	void OpenMenu();
	void CloseMenu();
	bool IsMenuOpen() const { return MenuWidget.IsValid(); }

protected:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void CreateSystemInput();
	void ToggleMenu();
	void OnQuitKey();
	void Quit();

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> SystemMappingContext;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MenuAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> QuitAction;

	TSharedPtr<SWidget> MenuWidget;
};
