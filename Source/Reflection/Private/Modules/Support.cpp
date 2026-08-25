/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Support.h"

#include "Engine/EngineUtilities.h"
#include "Utilities/Process.h"
#include "Engine/Notifications.h"
#include "Modules/UI/StyleModule.h"
#include "Settings/Static.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"

/* One turn of the clock. Every other one is quiet, so the ask itself comes round at twice this. */
static constexpr float TurnSeconds = 20.0f * 60.0f;

#if ENGINE_UE5
FTSTicker::FDelegateHandle FReflectionSupport::Handle;
#else
FDelegateHandle FReflectionSupport::Handle;
#endif

int32 FReflectionSupport::Turn = 0;

void FReflectionSupport::Register() {
	/* Nobody is reading notifications in a commandlet or on a server */
	if (IsRunningCommandlet() || IsRunningDedicatedServer() || !FSlateApplication::IsInitialized()) {
		return;
	}

	if (Handle.IsValid()) return;

	const FTickerDelegate Delegate = FTickerDelegate::CreateStatic(&FReflectionSupport::Tick);

#if ENGINE_UE5
	Handle = FTSTicker::GetCoreTicker().AddTicker(Delegate, TurnSeconds);
#else
	Handle = FTicker::GetCoreTicker().AddTicker(Delegate, TurnSeconds);
#endif
}

void FReflectionSupport::Unregister() {
	if (!Handle.IsValid()) return;

#if ENGINE_UE5
	FTSTicker::GetCoreTicker().RemoveTicker(Handle);
#else
	FTicker::GetCoreTicker().RemoveTicker(Handle);
#endif

	Handle.Reset();
}

bool FReflectionSupport::Tick(float DeltaTime) {
	/* The turn after an ask says nothing, which is what keeps it from being a nag */
	if (Turn++ % 2 == 0) {
		Show();
	}

	return true;
}

void FReflectionSupport::Show() {
	FNotificationInfo Info(FText::FromString("Enjoying Reflection?"));

	SetNotificationSubText(Info, FText::FromString(
		"Starring the repository helps other people find it."
	));

	Info.HyperlinkText = FText::FromString("Star on GitHub");
	Info.Hyperlink = FSimpleDelegate::CreateLambda([]() {
		LaunchURL(GitHub::URL);
	});

	Info.bFireAndForget = true;
	Info.FadeOutDuration = 0.1f;
	Info.ExpireDuration = 13.5f;
	Info.bUseLargeFont = false;
	Info.bUseThrobber = false;
	Info.Image = FReflectionStyle::Get().GetBrush("Toolbar.Icon");

	FSlateNotificationManager::Get().AddNotification(Info);
}
