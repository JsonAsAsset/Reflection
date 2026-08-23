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

/* Redirect Handler */
struct FRRedirects {
	static TMap<FString, TArray<FRRedirectorPoint>> History;

	static void Clear();
	static void Redirect(FString& Path);
	static void Reverse(FString& Path);
};