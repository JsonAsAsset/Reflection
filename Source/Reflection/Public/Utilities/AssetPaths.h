/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Settings/ReflectionSettings.h"
#include "Settings/Runtime.h"

/* The editor path for a Cloud one, with no redirect applied.
 *
 * Split out for anything that wants to say where a path would land without putting it through a
 * redirect, which is a thing Redirect records as having happened. */
inline FString ToEditorPackagePathDirect(const FString& InPath) {
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

	return Path;
}

/* Turns a package path as Cloud knows it into the one the same asset takes in the editor. */
inline FString ToEditorPackagePath(const FString& InPath) {
	FString Path = ToEditorPackagePathDirect(InPath);

	FRRedirects::Redirect(Path);

	return Path;
}

/* And back the other way, which is the only spelling Cloud answers to */
inline FString ToCloudPackagePath(const FString& InPath) {
	FString Path = InPath;

	/* Undone before it is turned back, so a path a redirect moved is asked for where it came from */
	FRRedirects::Reverse(Path);

	const FString& ProjectName = GReflectionRuntime.Profile.ProjectName;

	if (!ProjectName.IsEmpty() && Path.StartsWith(TEXT("/Game/"))) {
		Path = ProjectName + TEXT("/Content/") + Path.RightChop(6);
	}

	return Path;
}
