// Copyright Epic Games, Inc. All Rights Reserved.

#include "UGPTGameMode.h"
#include "UGPTHUD.h"
#include "UGPTCharacter.h"
#include "UObject/ConstructorHelpers.h"

AUGPTGameMode::AUGPTGameMode()
	: Super()
{
	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnClassFinder(TEXT("/Game/FirstPersonCPP/Blueprints/FirstPersonCharacter"));
	DefaultPawnClass = PlayerPawnClassFinder.Class;

	// use our custom HUD class
	HUDClass = AUGPTHUD::StaticClass();
}
