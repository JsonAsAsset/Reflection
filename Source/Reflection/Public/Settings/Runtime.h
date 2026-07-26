/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

struct FRCloudProfile {
	FString Name;
};

struct FRRuntime {
	/* UE4.22 ~~> 22 */
	int MinorVersion = -1;

	/* UE4.22 ~~> 4 */
	int MajorVersion = -1;

	FRCloudProfile Profile;
	FDirectoryPath ExportDirectory;

	bool bEnableToolbarToggling;

	/* Helper Functions ~~~~~~~~~~~ */
	bool IsOlderUE4Target() const;
	bool IsUE5() const;
	bool IsUE4() const;

	/* Update Functions ~~~~~~~~~~~ */
	void Update();
};

extern FRRuntime GReflectionRuntime;