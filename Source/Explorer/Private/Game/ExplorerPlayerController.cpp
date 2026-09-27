#include "Game/ExplorerPlayerController.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void AExplorerPlayerController::CreateSystemInput()
{
	if (SystemMappingContext)
	{
		return;
	}

	// Both actions must fire while the game is paused, since the menu pauses it.
	MenuAction = NewObject<UInputAction>(this, TEXT("IA_Menu"));
	MenuAction->bTriggerWhenPaused = true;
	QuitAction = NewObject<UInputAction>(this, TEXT("IA_Quit"));
	QuitAction->bTriggerWhenPaused = true;

	SystemMappingContext = NewObject<UInputMappingContext>(this, TEXT("IMC_System"));
	SystemMappingContext->MapKey(MenuAction, EKeys::Escape);
	SystemMappingContext->MapKey(MenuAction, EKeys::Gamepad_Special_Right);
	SystemMappingContext->MapKey(QuitAction, EKeys::Q);
}

void AExplorerPlayerController::BeginPlay()
{
	Super::BeginPlay();

	CreateSystemInput();
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		// Higher priority than the pawn's flight mapping.
		Subsystem->AddMappingContext(SystemMappingContext, 1);
	}
}

void AExplorerPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	CreateSystemInput();
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(InputComponent))
	{
		EnhancedInputComponent->BindAction(MenuAction, ETriggerEvent::Started, this, &AExplorerPlayerController::ToggleMenu);
		EnhancedInputComponent->BindAction(QuitAction, ETriggerEvent::Started, this, &AExplorerPlayerController::OnQuitKey);
	}
}

void AExplorerPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	CloseMenu();
	Super::EndPlay(EndPlayReason);
}

void AExplorerPlayerController::ToggleMenu()
{
	if (IsMenuOpen())
	{
		CloseMenu();
	}
	else
	{
		OpenMenu();
	}
}

void AExplorerPlayerController::OnQuitKey()
{
	if (IsInputKeyDown(EKeys::LeftAlt) || IsInputKeyDown(EKeys::RightAlt))
	{
		Quit();
	}
}

void AExplorerPlayerController::Quit()
{
	UKismetSystemLibrary::QuitGame(this, this, EQuitPreference::Quit, false);
}

void AExplorerPlayerController::OpenMenu()
{
	UGameViewportClient* Viewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr;
	if (IsMenuOpen() || !Viewport)
	{
		return;
	}

	auto MakeButton = [this](const FText& Label, void (AExplorerPlayerController::*Handler)())
	{
		return SNew(SBox)
			.WidthOverride(280.0f)
			.Padding(FMargin(0.0f, 6.0f))
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.ContentPadding(FMargin(16.0f, 10.0f))
				.OnClicked_Lambda([this, Handler]() { (this->*Handler)(); return FReply::Handled(); })
				[
					SNew(STextBlock)
					.Text(Label)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 20))
				]
			];
	};

	TSharedRef<SWidget> ResumeButton = MakeButton(INVTEXT("Resume"), &AExplorerPlayerController::CloseMenu);

	MenuWidget = SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.55f))
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 24.0f))
			[
				SNew(STextBlock)
				.Text(INVTEXT("Paused"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 36))
			]
			+ SVerticalBox::Slot().AutoHeight()[ResumeButton]
			+ SVerticalBox::Slot().AutoHeight()[MakeButton(INVTEXT("Quit"), &AExplorerPlayerController::Quit)]
		];

	Viewport->AddViewportWidgetContent(MenuWidget.ToSharedRef(), 100);

	FInputModeGameAndUI InputMode;
	InputMode.SetWidgetToFocus(ResumeButton);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	InputMode.SetHideCursorDuringCapture(false);
	SetInputMode(InputMode);
	SetShowMouseCursor(true);
	SetPause(true);
}

void AExplorerPlayerController::CloseMenu()
{
	if (!IsMenuOpen())
	{
		return;
	}

	if (UGameViewportClient* Viewport = GetWorld() ? GetWorld()->GetGameViewport() : nullptr)
	{
		Viewport->RemoveViewportWidgetContent(MenuWidget.ToSharedRef());
	}
	MenuWidget.Reset();

	SetPause(false);
	SetShowMouseCursor(false);
	SetInputMode(FInputModeGameOnly());
}
