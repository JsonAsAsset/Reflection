/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Support.h"

#include "Engine/EngineUtilities.h"
#include "Utilities/Process.h"
#include "Engine/Notifications.h"
#include "Modules/UI/StyleModule.h"
#include "Settings/Static.h"
#include "Settings/SettingsAccess.h"
#include "Modules/Cloud/Remote.h"

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

	/* Switched off, so there is no clock to turn */
	if (const UReflectionSettings* Settings = GetSettings(); Settings != nullptr && !Settings->ShowSupportPrompt) {
		return;
	}

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
	/* Switched off while the editor was open, so the clock stops now rather than at the next start */
	if (const UReflectionSettings* Settings = GetSettings(); Settings != nullptr && !Settings->ShowSupportPrompt) {
		Handle.Reset();

		return false;
	}

	/* The turn after an ask says nothing, which is what keeps it from being a nag */
	if (Turn % 2 != 0) {
		Turn++;

		return true;
	}

	/* Not while somebody is in the middle of something. */
	if (!IsQuiet()) return true;

	Turn++;

	Show();

	return true;
}

bool FReflectionSupport::IsQuiet() {
	/* Parked on a wait that is driving Slate itself */
	if (FBlockingRequestScope::IsActive()) return false;

	/* Something of Reflection's own is already on screen, and nobody is waiting on this one */

	/* [4.26] 	Under the 4.26.2 engine, we don't have access to IsEmpty on a TArray.
	 *			Getting Max == 0 *should* return the same functionality. */
	#if !UE4_26
	if (NotificationBudget::AtCap() || !NotificationBudget::Live.IsEmpty()) return false;
	#else
	if (NotificationBudget::AtCap() || NotificationBudget::Live.Max() != 0) return false;
	#endif

	/* Nobody in front of the editor to read it */
	if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().IsActive()) return false;

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

	/* Handed over the same way as every other notification here rather than straight to the manager.
	 *
	 * This one is asked for by a clock rather than by anything somebody did, so it can land on any
	 * call stack at all, including one a blocking wait is parked on and ticking Slate from. Built
	 * by that nested tick, it belongs to a list the outer tick has already finished with, and
	 * nothing drives it afterwards: it fades in and then sits there, because what would have faded
	 * it out is an Update nobody calls. */
	AddNotificationWhenSafe(Info, SNotificationItem::CS_None);
}
