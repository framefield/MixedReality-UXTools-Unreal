// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "UxtDefaultHandTrackerSubsystem.h"

#include "ARSupportInterface.h"
#include "IXRTrackingSystem.h"
#include "SceneViewExtension.h"
#include "UxtXRSimulationSubsystem.h"
#include "XRSimulationActor.h"

#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Features/IModularFeatures.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Utils/UxtFunctionLibrary.h"

void UUxtDefaultHandTrackerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	InputMappingContext = NewObject<UInputMappingContext>();
	InputMappingContext->AddToRoot();

	DefaultHandTracker.RegisterInputMappings(InputMappingContext);

	PostLoginHandle = FGameModeEvents::GameModePostLoginEvent.AddUObject(this, &UUxtDefaultHandTrackerSubsystem::OnGameModePostLogin);
	LogoutHandle = FGameModeEvents::GameModeLogoutEvent.AddUObject(this, &UUxtDefaultHandTrackerSubsystem::OnGameModeLogout);
}

void UUxtDefaultHandTrackerSubsystem::Deinitialize()
{
	FGameModeEvents::GameModePostLoginEvent.Remove(PostLoginHandle);
	FGameModeEvents::GameModeLogoutEvent.Remove(LogoutHandle);
	PostLoginHandle.Reset();
	LogoutHandle.Reset();

	DefaultHandTracker.UnregisterInputMappings(InputMappingContext);
}

void UUxtDefaultHandTrackerSubsystem::SetupForLocalPlayer(APlayerController* NewPlayer)
{
	if (NewPlayer->IsLocalController())
	{
		UEnhancedInputLocalPlayerSubsystem* EnhancedInputSystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(NewPlayer->GetLocalPlayer());
		if (EnhancedInputSystem && !EnhancedInputSystem->HasMappingContext(InputMappingContext))
		{
			EnhancedInputSystem->AddMappingContext(InputMappingContext, 0);
		}

		if (NewPlayer->InputComponent)
		{
			UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(NewPlayer->InputComponent);
			if (EnhancedInputComponent)
			{
				EnhancedInputComponent->BindAction(DefaultHandTracker.LeftSelect, ETriggerEvent::Started, this, 
					&UUxtDefaultHandTrackerSubsystem::OnLeftSelectPressed);
				EnhancedInputComponent->BindAction(DefaultHandTracker.LeftSelect, ETriggerEvent::Completed, this, 
					&UUxtDefaultHandTrackerSubsystem::OnLeftSelectReleased);

				EnhancedInputComponent->BindAction(DefaultHandTracker.LeftGrab, ETriggerEvent::Started, this, 
					&UUxtDefaultHandTrackerSubsystem::OnLeftGripPressed);
				EnhancedInputComponent->BindAction(DefaultHandTracker.LeftGrab, ETriggerEvent::Completed, this, 
					&UUxtDefaultHandTrackerSubsystem::OnLeftGripReleased);

				EnhancedInputComponent->BindAction(DefaultHandTracker.RightSelect, ETriggerEvent::Started, this, 
					&UUxtDefaultHandTrackerSubsystem::OnRightSelectPressed);
				EnhancedInputComponent->BindAction(DefaultHandTracker.RightSelect, ETriggerEvent::Completed, this, 
					&UUxtDefaultHandTrackerSubsystem::OnRightSelectReleased);

				EnhancedInputComponent->BindAction(DefaultHandTracker.RightGrab, ETriggerEvent::Started, this, 
					&UUxtDefaultHandTrackerSubsystem::OnRightGripPressed);
				EnhancedInputComponent->BindAction(DefaultHandTracker.RightGrab, ETriggerEvent::Completed, this, 
					&UUxtDefaultHandTrackerSubsystem::OnRightGripReleased);
			}
		}

		// Tick handler for updating the cached motion controller data.
		// Using OnWorldPreActorTick here which runs after OnWorldTickStart, at which point XR systems should have updated all controller
		// data.
		TickDelegateHandle = FWorldDelegates::OnWorldPreActorTick.AddUObject(this, &UUxtDefaultHandTrackerSubsystem::OnWorldPreActorTick);

		IModularFeatures::Get().RegisterModularFeature(IUxtHandTracker::GetModularFeatureName(), &DefaultHandTracker);
	}
}

void UUxtDefaultHandTrackerSubsystem::OnGameModePostLogin(AGameModeBase* GameMode, APlayerController* NewPlayer)
{
	SetupForLocalPlayer(NewPlayer);
}

void UUxtDefaultHandTrackerSubsystem::OnGameModeLogout(AGameModeBase* GameMode, AController* Exiting)
{
	if (APlayerController* PlayerController = Cast<APlayerController>(Exiting))
	{
		if (PlayerController->IsLocalController())
		{
			IModularFeatures::Get().UnregisterModularFeature(IUxtHandTracker::GetModularFeatureName(), &DefaultHandTracker);

			FWorldDelegates::OnWorldPreActorTick.Remove(TickDelegateHandle);

			if (PlayerController->InputComponent)
			{
				PlayerController->InputComponent->AxisBindings.RemoveAll(
					[this](const FInputAxisBinding& Binding) -> bool { return Binding.AxisDelegate.IsBoundToObject(this); });
			}
		}
	}
}

void UUxtDefaultHandTrackerSubsystem::OnWorldPreActorTick(UWorld* World, ELevelTick TickType, float DeltaTime)
{
	UUxtXRSimulationSubsystem* XRSimulationSubsystem = nullptr;
	if (APlayerController* PlayerController = World->GetFirstPlayerController())
	{
		if (ULocalPlayer* LocalPlayer = PlayerController->GetLocalPlayer())
		{
			XRSimulationSubsystem = LocalPlayer->GetSubsystem<UUxtXRSimulationSubsystem>();
		}
	}

	if (XRSimulationSubsystem && XRSimulationSubsystem->IsSimulationEnabled())
	{
		// Use simulated data generated by the simulation actor
		// Update Select/Grip state directly, no input events are used here
		XRSimulationSubsystem->GetMotionControllerData(
			EControllerHand::Left, DefaultHandTracker.ControllerData_Left, DefaultHandTracker.bIsSelectPressed_Left,
			DefaultHandTracker.bIsGrabbing_Left);
		XRSimulationSubsystem->GetMotionControllerData(
			EControllerHand::Right, DefaultHandTracker.ControllerData_Right, DefaultHandTracker.bIsSelectPressed_Right,
			DefaultHandTracker.bIsGrabbing_Right);

		// Head pose is using the XRTrackingSystem as well, force override in the function library
		FVector HeadPosition;
		FQuat HeadRotation;
		XRSimulationSubsystem->GetHeadPose(HeadRotation, HeadPosition);
		UUxtFunctionLibrary::bUseInputSim = true;
		UUxtFunctionLibrary::SimulatedHeadPose = FTransform(HeadRotation, HeadPosition);
	}
	else
	{
		// True XR system data from devices
		if (IXRTrackingSystem* XRSystem = GEngine->XRSystem.Get())
		{
			XRSystem->GetMotionControllerData(World, EControllerHand::Left, DefaultHandTracker.ControllerData_Left);
			XRSystem->GetMotionControllerData(World, EControllerHand::Right, DefaultHandTracker.ControllerData_Right);

			// Work around: tracking loss does not send a release event for Select/Grip
			if (DefaultHandTracker.ControllerData_Left.TrackingStatus == ETrackingStatus::NotTracked)
			{
				DefaultHandTracker.bIsSelectPressed_Left = false;
				DefaultHandTracker.bIsGrabbing_Left = false;
			}
			if (DefaultHandTracker.ControllerData_Right.TrackingStatus == ETrackingStatus::NotTracked)
			{
				DefaultHandTracker.bIsSelectPressed_Right = false;
				DefaultHandTracker.bIsGrabbing_Right = false;
			}
		}

		// Disable head pose override from simulation
		UUxtFunctionLibrary::bUseInputSim = false;
	}
}

void UUxtDefaultHandTrackerSubsystem::OnLeftSelectPressed()
{
	DefaultHandTracker.bIsSelectPressed_Left = true;
}

void UUxtDefaultHandTrackerSubsystem::OnLeftSelectReleased()
{
	DefaultHandTracker.bIsSelectPressed_Left = false;
}

void UUxtDefaultHandTrackerSubsystem::OnLeftGripPressed()
{
	DefaultHandTracker.bIsGrabbing_Left = true;
}

void UUxtDefaultHandTrackerSubsystem::OnLeftGripReleased()
{
	DefaultHandTracker.bIsGrabbing_Left = false;
}

void UUxtDefaultHandTrackerSubsystem::OnRightSelectPressed()
{
	DefaultHandTracker.bIsSelectPressed_Right = true;
}

void UUxtDefaultHandTrackerSubsystem::OnRightSelectReleased()
{
	DefaultHandTracker.bIsSelectPressed_Right = false;
}

void UUxtDefaultHandTrackerSubsystem::OnRightGripPressed()
{
	DefaultHandTracker.bIsGrabbing_Right = true;
}

void UUxtDefaultHandTrackerSubsystem::OnRightGripReleased()
{
	DefaultHandTracker.bIsGrabbing_Right = false;
}
