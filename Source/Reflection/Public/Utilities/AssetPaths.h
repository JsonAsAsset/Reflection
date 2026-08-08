/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Settings/ReflectionSettings.h"
#include "Settings/SettingsAccess.h"
#include "Settings/Runtime.h"

/* Turns a package path as Cloud knows it into the one the same asset takes in the editor. */
inline FString ToEditorPackagePath(const FString& InPath) {
	FString Path = InPath;

	const FString& ProjectName = GReflectionRuntime.Profile.ProjectName;

	if (!ProjectName.IsEmpty()) {
		Path = Path.Replace(*(ProjectName + "/Content/"), TEXT("/Game/"));
		Path = Path.Replace(*(ProjectName + "/Plugins"), TEXT(""));
		Path = Path.Replace(TEXT("/Content/"), TEXT("/"));
	}

	Path = Path.Replace(TEXT("Engine/Content"), TEXT("/Engine"));

	if (!Path.StartsWith(TEXT("/"))) {
		Path = "/" + Path;
	}

	FRRedirects::Redirect(Path);

	return Path;
}
