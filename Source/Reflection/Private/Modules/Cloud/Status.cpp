/* Copyright Reflection Contributors 2024-2026 */

#include "Reflection.h"
#include "Modules/Cloud/Cloud.h"
#include "Modules/UI/StyleModule.h"
#include "Engine/EngineUtilities.h"

static TWeakPtr<SNotificationItem> CloudNotification;

namespace {
	/* IsOpened decides whether the Cloud menu entries are enabled, so Slate asks for it every
	 * frame a menu is open. Walking the process table that often is a per-frame stall, and the
	 * Cloud starting or stopping is not something that needs frame-accurate reporting. */
	constexpr double OpenedCacheSeconds = 1.0;

	double GLastOpenedCheck = 0.0;
	bool GLastOpened = false;
}

bool Cloud::Status::IsOpened() {
	const double CurrentTime = FPlatformTime::Seconds();

	if (CurrentTime - GLastOpenedCheck < OpenedCacheSeconds) {
		return GLastOpened;
	}

	GLastOpenedCheck = CurrentTime;
	GLastOpened = IsProcessRunning(TEXT("Core.exe"));

	return GLastOpened;
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
