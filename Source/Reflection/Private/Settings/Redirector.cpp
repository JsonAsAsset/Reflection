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

	/* If there are specific profiles that go with this redirector, and the cloud is disabled, don't use any. */
	if (!GetSettings()->EnableCloudServer && Profiles.Num() > 0) {
		bIsEnabled = false;
	}
	
	return bIsEnabled;
}

void FRRedirects::Clear() {
	History.Empty();
}

void FRRedirects::Redirect(FString& Path) {
	TArray<FRRedirectorPoint> Points;

	const UReflectionSettings* Settings = GetSettings();

	for (const FRRedirector& Redirect : Settings->Redirectors) {
		if (!Redirect.IsEnabled()) continue;

		for (const FRRedirectorPoint& Point : Redirect.Points) {
			if (Path.Contains(Point.From)) {
				Points.Add(Point);
				
				Path = Path.Replace(*Point.From, *Point.To);
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

	for (const FRRedirectorPoint& Point : *Points) {
		Path = Path.Replace(*Point.To, *Point.From);
	}
}
