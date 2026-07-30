/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/ImportJob.h"

#include "Editor.h"
#include "TimerManager.h"
#include "UObject/StrongObjectPtr.h"

#include "Importers/Constructor/ImportReader.h"
#include "Modules/Log.h"
#include "Modules/UI/StyleModule.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"
#include "Modules/Cloud/Remote.h"

namespace {
	/* How long one tick may spend importing. Plenty of single exports run well past this and
	 * cannot be divided any further; the budget is here so that the small ones are not spread
	 * over a frame each for no reason. */
	constexpr double SliceBudgetSeconds = 0.008;

	/* Effectively "every tick". The timer manager treats a zero or negative rate as an error, so
	 * this is the smallest honest way to ask for one step per frame. */
	constexpr float StepRate = 0.001f;

	struct FJobState {
		TArray<FString> Files;
		int32 FileIndex = 0;

		/* The file currently being walked ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		FString CurrentFile;
		FUObjectExportContainer* Container = nullptr;
		int32 ExportIndex = 0;

		/* A file containing a BlueprintGeneratedClass imports only that, the same way
		 * IImportReader::ReadExportsAndImport decides it */
		bool bBlueprintOnly = false;

		int32 ImportedFiles = 0;

		/* Full frames happen between slices, which means garbage collection does too. The objects
		 * an export builds are reachable from a raw pointer in the container and not much else, so
		 * the job holds them itself until it is done. */
		TArray<TStrongObjectPtr<UObject>> Keep;
		TSet<UObject*> Kept;

		FTimerHandle Timer;
		TWeakPtr<SNotificationItem> Notification;
	};

	TUniquePtr<FJobState> GJob;

	bool IsPlayingInEditor() {
		if (GEditor == nullptr) {
			return false;
		}

		for (const FWorldContext& WorldContext : GEditor->GetWorldContexts()) {
			if (WorldContext.World() && WorldContext.World()->WorldType == EWorldType::PIE) {
				return true;
			}
		}

		return false;
	}

	void UpdateNotification() {
		const TSharedPtr<SNotificationItem> Item = GJob->Notification.Pin();
		if (!Item.IsValid()) {
			return;
		}

		Item->SetText(FText::FromString(FString::Printf(
			TEXT("Reflecting %d/%d: %s"),
			GJob->ImportedFiles + 1,
			GJob->Files.Num(),
			*FPaths::GetCleanFilename(GJob->CurrentFile)
		)));
	}

	/* Holds on to whatever the last export built, along with anything else the container has
	 * filled in along the way */
	void KeepContainerObjects() {
		if (GJob->Container == nullptr) {
			return;
		}

		for (const FUObjectExport* Export : GJob->Container->Exports) {
			if (Export == nullptr || Export->Object == nullptr) {
				continue;
			}

			if (GJob->Kept.Contains(Export->Object)) {
				continue;
			}

			GJob->Kept.Add(Export->Object);
			GJob->Keep.Add(TStrongObjectPtr<UObject>(Export->Object));
		}
	}

	/* Moves on to the next file when the current one is spent. False once there is nothing left. */
	bool OpenPendingFile() {
		if (GJob->Container != nullptr && GJob->Container->Exports.IsValidIndex(GJob->ExportIndex)) {
			return true;
		}

		/* The file that just finished */
		if (GJob->Container != nullptr) {
			GJob->Container = nullptr;
			GJob->ImportedFiles++;
		}

		while (GJob->Files.IsValidIndex(GJob->FileIndex)) {
			FString FilePath = GJob->Files[GJob->FileIndex++];

			if (FilePath.Contains("\\")) {
				FilePath = FilePath.Replace(TEXT("\\"), TEXT("/"));
			}

			TArray<TSharedPtr<FJsonValue>> DataObjects; {
				DeserializeJSON(FilePath, DataObjects);
			}

			if (DataObjects.Num() == 0) {
				UE_LOG(LogReflection, Warning, TEXT("Nothing to reflect from \"%s\"."), *FilePath);
				GJob->ImportedFiles++;

				continue;
			}

			/* Left alive deliberately. Importers keep hold of exports out of this container well
			 * past the call that made it, which is why nothing in Reflection deletes one. */
			FUObjectExportContainer* Container = new FUObjectExportContainer(DataObjects);

			/* Parsed, but nothing in it the container recognised as an export, so it is finished
			 * the moment it is opened. Returning it would step straight off the end. */
			if (Container->Exports.Num() == 0) {
				UE_LOG(LogReflection, Warning, TEXT("No exports found in \"%s\"."), *FilePath);
				GJob->ImportedFiles++;

				continue;
			}

			GJob->Container = Container;
			GJob->CurrentFile = FilePath;
			GJob->ExportIndex = 0;
			GJob->bBlueprintOnly = Container->FindByType(FString("BlueprintGeneratedClass"))->IsJsonValid();

			UpdateNotification();

			return true;
		}

		return false;
	}

	/* One frame's worth of importing. False once the job is done. */
	bool StepJob() {
		const double SliceStart = FPlatformTime::Seconds();

		/* Dependency fetches inside these exports still block, and this keeps them painted and
		 * cancellable while they do. Scoped to the slice, so no progress dialog is ever held
		 * across frames. */
		const FBlockingRequestScope BlockingScope(FText::FromString(TEXT("Reflecting from the Cloud")));

		do {
			if (!OpenPendingFile()) {
				return false;
			}

			FUObjectExport* Export = GJob->Container->Exports[GJob->ExportIndex++];

			if (Export == nullptr) continue;
			if (GJob->bBlueprintOnly && Export->GetType() != "BlueprintGeneratedClass") continue;

			IImportReader::ReadExportAndImport(GJob->Container, Export, GJob->CurrentFile);

			KeepContainerObjects();
		} while (FPlatformTime::Seconds() - SliceStart < SliceBudgetSeconds);

		return true;
	}

	void FinishJob() {
		if (GEditor != nullptr) {
			GEditor->GetTimerManager()->ClearTimer(GJob->Timer);
		}

		RemoveNotification(GJob->Notification);

		const int32 Count = GJob->ImportedFiles;

		/* Dropped before the notification below, so a handler that starts another import does not
		 * find this one still standing */
		GJob.Reset();

		UE_LOG(LogReflection, Log, TEXT("Reflection finished: %d file(s)."), Count);

		AppendNotification(
			FText::FromString(FString::Printf(TEXT("Reflected %d file(s)"), Count)),
			FText::FromString(TEXT("")),
			4.0f,
			FReflectionStyle::Get().GetBrush("Toolbar.Icon"),
			SNotificationItem::CS_Success,
			false,
			310.0f
		);
	}

	void TickJob() {
		if (!GJob.IsValid()) {
			return;
		}

		/* Building assets while the game is running in the editor is asking for trouble, so the
		 * job waits it out rather than racing PIE. Said out loud, because a progress line that
		 * stops moving on its own reads exactly like the hang this is meant to avoid. */
		if (IsPlayingInEditor()) {
			if (const TSharedPtr<SNotificationItem> Item = GJob->Notification.Pin()) {
				Item->SetText(FText::FromString(TEXT("Reflection paused while in Play In Editor")));
			}

			return;
		}

		if (!StepJob()) {
			FinishJob();

			return;
		}

		UpdateNotification();
	}
}

void FImportJob::Enqueue(const TArray<FString>& Files) {
	if (Files.Num() == 0) {
		return;
	}

	/* No editor loop to slice against, so there is nothing to hand a frame back to */
	if (GEditor == nullptr) {
		for (const FString& File : Files) {
			IImportReader::ImportReference(File);
		}

		return;
	}

	if (GJob.IsValid()) {
		GJob->Files.Append(Files);
		UpdateNotification();

		return;
	}

	GJob = MakeUnique<FJobState>();
	GJob->Files = Files;

	GJob->Notification = AppendNotificationWithHandler(
		FText::FromString(TEXT("Reflecting")),
		FText::FromString(TEXT("")),
		999.0f,
		FReflectionStyle::Get().GetBrush("Toolbar.Icon"),
		SNotificationItem::CS_Pending,
		false,
		340.0f
	);

	GEditor->GetTimerManager()->SetTimer(
		GJob->Timer,
		FTimerDelegate::CreateStatic(&TickJob),
		StepRate,
		true
	);
}

bool FImportJob::IsRunning() {
	return GJob.IsValid();
}

void FImportJob::Cancel() {
	if (!GJob.IsValid()) {
		return;
	}

	/* The export in flight is left to finish; only what has not been started is dropped */
	GJob->Files.SetNum(GJob->FileIndex);

	if (GJob->Container != nullptr) {
		GJob->ExportIndex = GJob->Container->Exports.Num();
	}
}
