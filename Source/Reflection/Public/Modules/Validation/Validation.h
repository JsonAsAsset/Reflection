/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/Compatibility.h"

/* Validation is UE5 only */
#if ENGINE_UE5

/* Result of checking one project asset against the real game files */
enum class EValidationStatus : uint8 {
	/* Waiting on the Cloud */
	Pending,

	/* The package exists in the game at this exact path */
	Valid,

	/* The package isn't here, but an asset of the same name lives somewhere else in the game */
	Misplaced,

	/* Nothing in the game carries this name */
	Missing
};

REFLECTION_API FText GetValidationStatusText(EValidationStatus Status);
REFLECTION_API FLinearColor GetValidationStatusColor(EValidationStatus Status);

/* One validated asset */
struct REFLECTION_API FValidationEntry {
	/* Package path inside the project, ex: "/Game/Characters/Items/Weapon" */
	FString PackagePath;

	/* Trailing name of the package, ex: "Weapon" */
	FString AssetName;

	/* Class of the asset as reported by the Asset Registry */
	FString ClassName;

	/* Where the Cloud looked for it, as a path into the mounted game files */
	FString ResolvedPath;

	/* Real locations of an asset with this name, only filled in when the package is missing */
	TArray<FString> Suggestions;

	EValidationStatus Status = EValidationStatus::Pending;

	/* The location this asset most likely belongs at, empty unless misplaced */
	const FString& GetSuggestedPath() const;

	/* Folder the asset should be moved into, ex: "/Game/Correct/Folder" */
	FString GetSuggestedFolder() const;
};

/* Walks a content folder and asks the Cloud whether each package is where it belongs.
 *
 * Assets are sent to "/api/validate" in batches, a handful of requests at a time, so the
 * editor stays responsive while thousands of packages are checked. */
class REFLECTION_API FValidation : public TSharedFromThis<FValidation> {
public:
	/* Packages sent per request */
	static constexpr int32 BatchSize = 400;

	/* Requests allowed to be waiting on the Cloud at once */
	static constexpr int32 MaxRequestsInFlight = 4;

	/* Collects every asset under RootPath and starts checking them. Any validation already in progress is dropped. */
	void Start(const FString& RootPath);

	/* Stops dispatching new batches. Responses to requests already sent are ignored. */
	void Cancel();

	bool IsRunning() const { return bRunning; }

	/* Assets checked so far over assets found, as a 0-1 fraction */
	float GetProgress() const;

	int32 GetCheckedCount() const { return CheckedCount; }
	int32 GetCount(EValidationStatus Status) const;

	const TArray<TSharedPtr<FValidationEntry>>& GetEntries() const { return Entries; }

	/* Why the last validation stopped early, empty if it went fine */
	const FString& GetFailureReason() const { return FailureReason; }

	/* Raised whenever results change, and once more when validation ends */
	FSimpleMulticastDelegate OnUpdated;

private:
	void GatherAssets(const FString& RootPath);
	void DispatchBatches();
	void HandleBatchResponse(int32 BatchStart, int32 BatchEnd, const TSharedPtr<FJsonObject>& Response, int32 ResponseCode);
	void Finish(const FString& Reason);

	TArray<TSharedPtr<FValidationEntry>> Entries;

	/* Package path -> entry, so responses can be matched back no matter what order they arrive in */
	TMap<FString, TSharedPtr<FValidationEntry>> EntryLookup;

	/* Index of the first entry not yet handed to a request */
	int32 NextBatchStart = 0;

	int32 CheckedCount = 0;
	int32 RequestsInFlight = 0;

	/* Bumped on every Start/Cancel so responses from an abandoned validation can be dropped */
	uint32 RunId = 0;

	bool bRunning = false;
	FString FailureReason;
};

#endif
