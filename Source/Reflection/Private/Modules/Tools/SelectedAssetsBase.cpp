/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Tools/SelectedAssetsBase.h"

#include "Modules/Cloud/Cloud.h"
#include "Modules/UI/StyleModule.h"
#include "Engine/EngineUtilities.h"

void TSelectedAssetsBase::Execute() {
	if (bRunning) {
		AppendNotification(
			FText::FromString("Already Running"),
			FText::FromString("Wait for the current Cloud run to finish."),
			3.0f,
			FReflectionStyle::Get().GetBrush("Toolbar.Icon"),
			SNotificationItem::CS_None,
			false,
			310.0f
		);

		return;
	}

	TArray<FAssetData> AssetDataList = GetAssetsInSelectedFolder();

	/* One selected asset is a more specific request than the folder it happens to sit in */
	if (const UObject* SelectedAsset = GetSelectedAsset<UObject>(true)) {
		AssetDataList.Empty();
		AssetDataList.Add(FAssetData(SelectedAsset));
	}

	const FName SupportedClass = GetSupportedClass();

	Queue.Reset();

	for (const FAssetData& AssetData : AssetDataList) {
		if (!AssetData.IsValid()) continue;
		if (!SupportedClass.IsNone() && GetAssetDataClass(AssetData) != SupportedClass) continue;

		const FString ObjectPath = GetAssetObjectPath(AssetData);
		if (ObjectPath.IsEmpty()) continue;

		Queue.Add(ObjectPath);
	}

	if (Queue.Num() == 0) {
		return;
	}

	QueueIndex = 0;
	bRunning = true;

	RequestNext();
}

void TSelectedAssetsBase::RequestNext() {
	if (!Queue.IsValidIndex(QueueIndex)) {
		Finish();

		return;
	}

	/* One request at a time: Process deserializes into live assets, and the tools were written
	 * expecting to be the only thing doing that */
	Cloud::Export::GetRawExportsAsync(Queue[QueueIndex], [this](const TArray<TSharedPtr<FJsonValue>>& Exports) {
		/* Copied out of the response, which does not outlive a deferred call */
		const TArray<TSharedPtr<FJsonValue>> OwnedExports = Exports;

		Cloud::RunWhenSafe([this, OwnedExports]() {
			HandleExports(OwnedExports);
		});
	});
}

void TSelectedAssetsBase::HandleExports(const TArray<TSharedPtr<FJsonValue>>& Exports) {
	const FString ObjectPath = Queue.IsValidIndex(QueueIndex) ? Queue[QueueIndex] : FString();

	QueueIndex++;

	if (Exports.Num() > 0 && !ObjectPath.IsEmpty()) {
		/* Looked up now rather than held from Execute: the asset could have been unloaded
		 * while the request was out */
		if (UObject* Object = LoadObjectByPath<UObject>(ObjectPath)) {
			/* The serializers are plain NewObject's that nothing keeps alive, and a run spans
			 * enough ticks for a collection to happen between two assets. Building them here
			 * puts their whole lifetime inside one call stack, and hands each asset a
			 * serializer with no leftover state from the last one. */
			CreateSerializer();

			Process(Object, Exports);
		}
	}

	RequestNext();
}

void TSelectedAssetsBase::BrowseToWhenFinished(const UObject* Object) {
	if (Object == nullptr) return;

	PendingBrowseTo = Object->GetPathName();
}

void TSelectedAssetsBase::Finish() {
	Queue.Reset();

	QueueIndex = 0;
	bRunning = false;

	if (!PendingBrowseTo.IsEmpty()) {
		/* Looked up now rather than held from Process: the asset could have been unloaded since */
		if (UObject* Object = LoadObjectByPath<UObject>(PendingBrowseTo)) {
			BrowseToAsset(Object);
		}

		PendingBrowseTo.Empty();
	}
}
