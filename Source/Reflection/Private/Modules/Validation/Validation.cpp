/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Validation/Validation.h"

/* Validation is UE5 only */
#if ENGINE_UE5

#include "Modules/Cloud/Cloud.h"
#include "Modules/Log.h"
#include "Engine/EngineUtilities.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "Reflection.Validation"

FText GetValidationStatusText(const EValidationStatus Status) {
	switch (Status) {
		case EValidationStatus::Valid: return LOCTEXT("StatusValid", "Valid");
		case EValidationStatus::Misplaced: return LOCTEXT("StatusMisplaced", "Misplaced");
		case EValidationStatus::Missing: return LOCTEXT("StatusMissing", "Missing");
		default: return LOCTEXT("StatusPending", "Pending");
	}
}

FLinearColor GetValidationStatusColor(const EValidationStatus Status) {
	switch (Status) {
		case EValidationStatus::Valid: return FLinearColor(0.30f, 0.75f, 0.35f);
		case EValidationStatus::Misplaced: return FLinearColor(0.95f, 0.66f, 0.20f);
		case EValidationStatus::Missing: return FLinearColor(0.85f, 0.30f, 0.30f);
		default: return FLinearColor(0.55f, 0.55f, 0.55f);
	}
}

const FString& FValidationEntry::GetSuggestedPath() const {
	static const FString Empty;

	return Suggestions.Num() > 0 ? Suggestions[0] : Empty;
}

FString FValidationEntry::GetSuggestedFolder() const {
	const FString& Suggestion = GetSuggestedPath();
	if (Suggestion.IsEmpty()) {
		return FString();
	}

	FString Folder;
	if (!Suggestion.Split(TEXT("/"), &Folder, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
		return FString();
	}

	return Folder;
}

void FValidation::Start(const FString& RootPath) {
	Cancel();

	Entries.Reset();
	EntryLookup.Reset();

	NextBatchStart = 0;
	CheckedCount = 0;
	RequestsInFlight = 0;
	FailureReason.Empty();

	GatherAssets(RootPath.IsEmpty() ? TEXT("/Game") : RootPath);

	if (Entries.Num() == 0) {
		FailureReason = FString::Printf(TEXT("No assets found under '%s'."), *RootPath);
		OnUpdated.Broadcast();

		return;
	}

	bRunning = true;

	OnUpdated.Broadcast();
	DispatchBatches();
}

void FValidation::Cancel() {
	/* Responses still in flight belong to the previous run and get dropped on arrival */
	RunId++;

	bRunning = false;
	RequestsInFlight = 0;
}

float FValidation::GetProgress() const {
	if (Entries.Num() == 0) {
		return 0.0f;
	}

	return static_cast<float>(CheckedCount) / static_cast<float>(Entries.Num());
}

int32 FValidation::GetCount(const EValidationStatus Status) const {
	int32 Count = 0;

	for (const TSharedPtr<FValidationEntry>& Entry : Entries) {
		if (Entry->Status == Status) {
			Count++;
		}
	}

	return Count;
}

void FValidation::GatherAssets(const FString& RootPath) {
	const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	const IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FAssetData> AssetDataList;
	AssetRegistry.GetAssetsByPath(FName(*RootPath), AssetDataList, true);

	Entries.Reserve(AssetDataList.Num());
	EntryLookup.Reserve(AssetDataList.Num());

	for (const FAssetData& AssetData : AssetDataList) {
		if (!AssetData.IsValid()) {
			continue;
		}

		const FString PackagePath = AssetData.PackageName.ToString();

		/* One World Partition level generates thousands of these, and none of them are shipped
		 * as standalone packages, so validating them would only ever produce noise */
		if (PackagePath.Contains(TEXT("/__External"))) {
			continue;
		}

		/* Multiple assets can share a package, and the package is what gets validated */
		if (EntryLookup.Contains(PackagePath)) {
			continue;
		}

		TSharedPtr<FValidationEntry> Entry = MakeShared<FValidationEntry>();
		Entry->PackagePath = PackagePath;
		Entry->AssetName = FPackageName::GetShortName(PackagePath);
		Entry->ClassName = GetAssetDataClass(AssetData).ToString();

		Entries.Add(Entry);
		EntryLookup.Add(PackagePath, Entry);
	}

	Entries.Sort([](const TSharedPtr<FValidationEntry>& A, const TSharedPtr<FValidationEntry>& B) {
		return A->PackagePath < B->PackagePath;
	});
}

void FValidation::DispatchBatches() {
	const uint32 DispatchRunId = RunId;

	while (bRunning && RequestsInFlight < MaxRequestsInFlight && NextBatchStart < Entries.Num()) {
		const int32 BatchStart = NextBatchStart;
		const int32 BatchEnd = FMath::Min(BatchStart + BatchSize, Entries.Num());

		NextBatchStart = BatchEnd;
		RequestsInFlight++;

		FString Body; {
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);

			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("suggest"), true);
			Writer->WriteValue(TEXT("maxSuggestions"), 4);
			Writer->WriteArrayStart(TEXT("paths"));

			for (int32 Index = BatchStart; Index < BatchEnd; Index++) {
				Writer->WriteValue(Entries[Index]->PackagePath);
			}

			Writer->WriteArrayEnd();
			Writer->WriteObjectEnd();
			Writer->Close();
		}

		TWeakPtr<FValidation> WeakValidation = AsShared();

		Cloud::Post(TEXT("/api/validate"), Body, {},
			[WeakValidation, DispatchRunId, BatchStart, BatchEnd](const TSharedPtr<FJsonObject>& Response, const int32 ResponseCode) {
				const TSharedPtr<FValidation> Validation = WeakValidation.Pin();

				/* The panel could have been closed, or a new validation started, while this was in flight */
				if (!Validation.IsValid() || Validation->RunId != DispatchRunId) {
					return;
				}

				Validation->RequestsInFlight--;
				Validation->HandleBatchResponse(BatchStart, BatchEnd, Response, ResponseCode);
			}
		);
	}
}

void FValidation::HandleBatchResponse(const int32 BatchStart, const int32 BatchEnd, const TSharedPtr<FJsonObject>& Response, const int32 ResponseCode) {
	if (!Response.IsValid()) {
		if (ResponseCode == 404) {
			Finish(TEXT("This Cloud build has no '/api/validate' endpoint. Update Core to run validation."));
		} else if (ResponseCode == 0) {
			Finish(TEXT("Couldn't reach the Cloud. Make sure Core is running and a profile is loaded."));
		} else {
			Finish(FString::Printf(TEXT("The Cloud rejected the validation request (HTTP %d)."), ResponseCode));
		}

		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* Results;
	if (!Response->TryGetArrayField(TEXT("results"), Results)) {
		Finish(TEXT("The Cloud returned a response without any results."));

		return;
	}

	for (const TSharedPtr<FJsonValue>& Result : *Results) {
		const TSharedPtr<FJsonObject>* ResultObject;
		if (!Result.IsValid() || !Result->TryGetObject(ResultObject)) {
			continue;
		}

		FString PackagePath;
		if (!(*ResultObject)->TryGetStringField(TEXT("path"), PackagePath)) {
			continue;
		}

		const TSharedPtr<FValidationEntry>* Found = EntryLookup.Find(PackagePath);
		if (Found == nullptr) {
			continue;
		}

		const TSharedPtr<FValidationEntry>& Entry = *Found;

		/* A duplicated path in the response must not be counted twice */
		if (Entry->Status != EValidationStatus::Pending) {
			continue;
		}

		(*ResultObject)->TryGetStringField(TEXT("resolved"), Entry->ResolvedPath);

		const TArray<TSharedPtr<FJsonValue>>* Matches;
		if ((*ResultObject)->TryGetArrayField(TEXT("matches"), Matches)) {
			for (const TSharedPtr<FJsonValue>& Match : *Matches) {
				FString MatchPath;

				if (Match.IsValid() && Match->TryGetString(MatchPath) && !MatchPath.IsEmpty()) {
					Entry->Suggestions.Add(MatchPath);
				}
			}
		}

		bool bExists = false;
		(*ResultObject)->TryGetBoolField(TEXT("exists"), bExists);

		if (bExists) {
			Entry->Status = EValidationStatus::Valid;
		} else {
			Entry->Status = Entry->Suggestions.Num() > 0 ? EValidationStatus::Misplaced : EValidationStatus::Missing;
		}

		CheckedCount++;
	}

	/* Anything the Cloud skipped still has to move off Pending, or validation would never end */
	for (int32 Index = BatchStart; Index < BatchEnd; Index++) {
		const TSharedPtr<FValidationEntry>& Entry = Entries[Index];

		if (Entry->Status == EValidationStatus::Pending) {
			Entry->Status = EValidationStatus::Missing;
			CheckedCount++;
		}
	}

	if (CheckedCount >= Entries.Num()) {
		Finish(FString());

		return;
	}

	OnUpdated.Broadcast();
	DispatchBatches();
}

void FValidation::Finish(const FString& Reason) {
	if (!Reason.IsEmpty()) {
		FailureReason = Reason;
		UE_LOG(LogReflection, Warning, TEXT("Validation stopped: %s"), *Reason);
	}

	Cancel();
	OnUpdated.Broadcast();
}

#undef LOCTEXT_NAMESPACE

#endif
