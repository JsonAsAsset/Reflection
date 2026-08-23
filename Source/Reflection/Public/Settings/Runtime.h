/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

struct FRCloudProfile {
	/* What the profile is called in Cloud, which is whatever the user named it */
	FString Name;

	/* The project the profile's files came out of, which is what package paths are spelled with. Nothing like the name above, and the only one of the two paths care. */
	FString ProjectName;
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