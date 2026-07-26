/* Copyright Reflection Contributors 2024-2026 */

#include "Reflection.h"
#include "Modules/Cloud/Cloud.h"
#include "Modules/UI/StyleModule.h"
#include "Engine/EngineUtilities.h"

static TWeakPtr<SNotificationItem> CloudNotification;

bool Cloud::Status::IsOpened() {
	return IsProcessRunning("Core.exe");
}

void Cloud::Status::IsReady(TFunction<void(bool)> OnResponse) {
	Get("/api/status", {}, {},
		[OnResponse](const TSharedPtr<FJsonObject>& Json) {
			OnResponse(Json.IsValid());
		}
	);
}

void Cloud::Status::Check(const UReflectionSettings* Settings,TFunction<void(bool)> OnResponse) {
	RemoveNotification(CloudNotification);
	
	if (Settings->EnableCloudServer && !IsOpened()) {
		CloudNotification = AppendNotificationWithHandler(
			FText::FromString("No Active Cloud Instance"),
			FText::FromString("Read documentation on how to start one."),
			0.5f,
			FReflectionStyle::Get().GetBrush("Toolbar.Icon"),
			SNotificationItem::CS_None,
			false,
			0.0f,
			[](FNotificationInfo& Info) {
				Info.HyperlinkText = FText::FromString("Learn how to setup");
				Info.Hyperlink = FSimpleDelegate::CreateStatic([]() {
					LaunchURL(GitHub::README::Cloud);
				});
			}
		);
		
		OnResponse(false);
		
		return;
	}

	IsReady([OnResponse](const bool bReady) {
		OnResponse(bReady);
	});
}

bool Cloud::Status::ShouldWaitUntilInitialized(const UReflectionSettings* Settings) {
	return Settings->EnableCloudServer && IsOpened();
}
