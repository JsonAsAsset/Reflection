/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Redirector.generated.h"

/* A point in a redirector */
USTRUCT()
struct FRRedirectorPoint {
	GENERATED_BODY()
public:
	/* Substring to match in the path (case-sensitive).
	 * Supports folder, package, or full asset paths.
	 * Example: "/Game/A/" */
	UPROPERTY(EditAnywhere, Config, Category = RedirectorPoint)
	FString From;

	/* Substring that replaces `From` when matched.
	 * Replaces all occurrences in the path.
	 * Example: "/Game/B/" */
	UPROPERTY(EditAnywhere, Config, Category = RedirectorPoint)
	FString To;
};

USTRUCT()
struct FRRedirector {
	GENERATED_BODY()
public:
	bool IsEnabled() const;

	/* What this one is called, which is all anything showing a moved path can say about why */
	FName GetName() const { return Name; }
	void SetName(const FName InName) { Name = InName; }

	/* The toggle on its own, which is not the same question as IsEnabled: that one also asks
	 * whether this redirector is meant for the profile that is loaded */
	bool IsOn() const { return Enable; }
	void SetOn(const bool bOn) { Enable = bOn; }

	/* The profiles this one is kept to, empty for all of them */
	const TArray<FString>& GetProfiles() const { return Profiles; }
	void SetProfiles(const TArray<FString>& InProfiles) { Profiles = InProfiles; }

private:
	/* The name of this redirector. */
	UPROPERTY(EditAnywhere, Config, Category = Redirector)
	FName Name;
	
	/* Master toggle for this redirector.
	 * If false, this redirector is always disabled regardless of any other setting. */
	UPROPERTY(EditAnywhere, Config, Category = Redirector)
	bool Enable = true;

public:
	/* Ordered list of redirect rules applied to the path.
	 * Each point can modify the result of the previous one. */
	UPROPERTY(EditAnywhere, Config, Category = Redirector)
	TArray<FRRedirectorPoint> Points;

private:
	/* Optional whitelist of profile names that can use this redirector.
	 * Compared against the current cloud profile (exact match).
	 *
	 * If empty, the redirector is enabled for all profiles. */
	UPROPERTY(EditAnywhere, Config, Category = Metadata)
	TArray<FString> Profiles;
};

/* What a redirect would do to a path, without doing it. */
struct FRRedirectPreview {
	/* The path every matching point left behind, which is where the asset lands */
	FString Path;

	/* The redirectors that moved it, in the order they were applied */
	TArray<FName> By;

	/* The same ones by where they sit in the settings, for anything that has to tell two apart
	 * when both are called the same or neither is called anything */
	TArray<int32> Indices;

	bool Moved() const { return By.Num() > 0; }
};

/* Redirect Handler */
struct FRRedirects {
	static TMap<FString, TArray<FRRedirectorPoint>> History;

	static void Clear();
	static void Redirect(FString& Path);
	static void Reverse(FString& Path);

	/* Where a path would end up, for anything that only means to show it.
	 *
	 * Redirect keeps a history so a moved path can be turned back into the one Cloud answers to.
	 * Nothing is being imported here, so there would be nothing to turn back, and a history entry
	 * for a path that was never fetched is one Reverse can later find instead of the real one. */
	static FRRedirectPreview Preview(const FString& Path);
};