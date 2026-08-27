/* Copyright Reflection Contributors 2024-2026 */

#include "Settings/Redirector.h"

#include "Settings/ReflectionSettings.h"
#include "Settings/Runtime.h"
#include "Engine/EngineUtilities.h"

/********* Redirect History ************ */
TMap<FString, TArray<FRRedirectorPoint>> FRRedirects::History;

bool FRRedirector::IsEnabled() const {
	bool bIsEnabled = Enable;

	if (!GReflectionRuntime.Profile.Name.IsEmpty()) {
		if (Profiles.Num() > 0 && !Profiles.Contains(GReflectionRuntime.Profile.Name)) {
			bIsEnabled = false;
		}
	}

	return bIsEnabled;
}

void FRRedirects::Clear() {
	History.Empty();
}

/* The first time a path says something, and only the first.
 *
 * A folder name is not unique down a path: "/Game/Folder/Game/" says Game twice, and the one that
 * was meant is the one at the front the rest are folders somebody named, which happen to read
 * the same. Replaced everywhere, redirecting /Game/ rewrites both and the asset is looked for
 * somewhere that does not exist.
 *
 * The first rather than the front, since a redirect is not always rooted: "Animation/Game" is a
 * way down into a path and has the same trouble with the same folder. */
static bool ReplaceFirst(FString& Path, const FString& From, const FString& To) {
	if (From.IsEmpty()) return false;

	const int32 At = Path.Find(From, ESearchCase::IgnoreCase, ESearchDir::FromStart);

	if (At == INDEX_NONE) return false;

	Path = Path.Left(At) + To + Path.RightChop(At + From.Len());

	return true;
}

void FRRedirects::Redirect(FString& Path) {
	TArray<FRRedirectorPoint> Points;

	const UReflectionSettings* Settings = GetSettings();

	for (const FRRedirector& Redirect : Settings->Redirectors) {
		if (!Redirect.IsEnabled()) continue;

		for (const FRRedirectorPoint& Point : Redirect.Points) {
			if (ReplaceFirst(Path, Point.From, Point.To)) {
				Points.Add(Point);
			}
		}
	}

	TArray<FRRedirectorPoint>& Pointers = History.FindOrAdd(Path);
	Pointers.Append(Points);
}

void FRRedirects::Reverse(FString& Path) {
	TArray<FRRedirectorPoint>* Points = History.Find(Path);
	if (!Points) {
		return;
	}

	/* Undone the way it was done, one for one and last first */
	for (int32 At = Points->Num() - 1; At >= 0; --At) {
		const FRRedirectorPoint& Point = (*Points)[At];

		ReplaceFirst(Path, Point.To, Point.From);
	}
}
