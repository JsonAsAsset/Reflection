/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#include "Editor.h"
#include "TimerManager.h"
#include "Modules/Cloud/Remote.h"

/* How many notifications Reflection is allowed to have going at once.
 *
 * Every notification is a Slate window, and one import reaches as many references as it reaches:
 * a material with a few hundred of them asks for a few hundred windows inside a couple of frames,
 * and the editor spends longer building those than it does importing. Past the cap the extras are
 * counted rather than shown, and the count rides along on the next notification that gets through
 * once the earlier ones have expired. */
namespace NotificationBudget {
	constexpr int32 MaxLive = 6;

	/* Already on screen. Weak: the manager owns them, and the handle going stale is how one
	 * expiring is noticed without anything having to tick. */
	inline TArray<TWeakPtr<SNotificationItem>> Live;

	/* Queued for the next tick and not built yet. Counted because during an import every
	 * notification takes that route, so without this the cap only sees an empty screen. */
	inline int32 Pending = 0;

	inline int32 Suppressed = 0;

	inline bool AtCap() {
		Live.RemoveAll([](const TWeakPtr<SNotificationItem>& Item) {
			return !Item.IsValid();
		});

		return Live.Num() + Pending >= MaxLive;
	}

	inline void Track(const TSharedPtr<SNotificationItem>& Item) {
		if (Item.IsValid()) {
			Live.Add(Item);
		}
	}
}

/* ReSharper disable once CppParameterNeverUsed */
inline void SetNotificationSubText(FNotificationInfo& Notification, const FText& SubText) {
#if ENGINE_UE5
	Notification.SubText = SubText;
#endif
}

/* Hands a notification to the manager on a call stack that owns itself.
 *
 * A notification is a Slate window, and a blocking Cloud wait keeps the editor painting by
 * ticking Slate from inside whatever call stack it is parked on. One added from there gets built
 * by that nested tick and torn down by the outer one, and the double free lands seconds later
 * when the notification expires, nowhere near the code that caused it. */
inline TSharedPtr<SNotificationItem> AddNotificationWhenSafe(const FNotificationInfo& Info, const SNotificationItem::ECompletionState CompletionState) {
	if (FBlockingRequestScope::IsActive() && GEditor != nullptr) {
		NotificationBudget::Pending++;

		GEditor->GetTimerManager()->SetTimerForNextTick(FTimerDelegate::CreateLambda([Info, CompletionState] {
			NotificationBudget::Pending--;

			if (const TSharedPtr<SNotificationItem> DeferredItem = FSlateNotificationManager::Get().AddNotification(Info)) {
				DeferredItem->SetCompletionState(CompletionState);

				NotificationBudget::Track(DeferredItem);
			}
		}));

		return nullptr;
	}

	const TSharedPtr<SNotificationItem> NotificationPtr = FSlateNotificationManager::Get().AddNotification(Info);

	if (NotificationPtr.IsValid()) {
		NotificationPtr->SetCompletionState(CompletionState);

		NotificationBudget::Track(NotificationPtr);
	}

	return NotificationPtr;
}

/* Carries whatever the cap held back onto the next notification that gets through, so a flood
 * leaves a trace rather than silently going missing */
inline void SetNotificationSubTextWithSuppressed(FNotificationInfo& Info, const FText& SubText) {
	if (NotificationBudget::Suppressed == 0) {
		SetNotificationSubText(Info, SubText);

		return;
	}

	const FText Note = FText::Format(
		NSLOCTEXT("Reflection", "NotificationsHidden", "{0} ({1} more hidden)"),
		SubText,
		FText::AsNumber(NotificationBudget::Suppressed)
	);

	NotificationBudget::Suppressed = 0;

	SetNotificationSubText(Info, Note);
}

/* Show the user a Notification */
inline auto AppendNotification(const FText& Text, const FText& SubText, const float ExpireDuration,
                               const SNotificationItem::ECompletionState CompletionState, const bool UseSuccessFailIcons,
                               const float WidthOverride) -> void
{
	/* Nobody is waiting on this one, so it is the kind the cap is allowed to drop */
	if (NotificationBudget::AtCap()) {
		NotificationBudget::Suppressed++;

		return;
	}

	FNotificationInfo Info = FNotificationInfo(Text);
	Info.ExpireDuration = ExpireDuration;
	Info.bUseLargeFont = true;
	Info.bUseSuccessFailIcons = UseSuccessFailIcons;
	Info.WidthOverride = FOptionalSize(WidthOverride);

	SetNotificationSubTextWithSuppressed(Info, SubText);

	AddNotificationWhenSafe(Info, CompletionState);
}

/* Show the user a Notification with Subtext */
inline auto AppendNotification(const FText& Text, const FText& SubText, float ExpireDuration,
                               const FSlateBrush* SlateBrush, SNotificationItem::ECompletionState CompletionState,
                               const bool UseSuccessFailIcons, const float WidthOverride) -> void
{
	/* Nobody is waiting on this one, so it is the kind the cap is allowed to drop */
	if (NotificationBudget::AtCap()) {
		NotificationBudget::Suppressed++;

		return;
	}

	FNotificationInfo Info = FNotificationInfo(Text);
	Info.ExpireDuration = ExpireDuration;
	Info.bUseLargeFont = true;
	Info.bUseSuccessFailIcons = UseSuccessFailIcons;
	Info.WidthOverride = FOptionalSize(WidthOverride);
	Info.Image = SlateBrush;

	SetNotificationSubTextWithSuppressed(Info, SubText);

	AddNotificationWhenSafe(Info, CompletionState);
}

inline TSharedPtr<SNotificationItem> AppendNotificationWithHandler(const FText& Text, const FText& SubText, const float ExpireDuration,
	const FSlateBrush* SlateBrush, const SNotificationItem::ECompletionState CompletionState, const bool UseSuccessFailIcons,
	const float WidthOverride, const TFunction<void(FNotificationInfo&)>& PreAddHandler = nullptr)
{
	FNotificationInfo Info(Text);
	Info.ExpireDuration = ExpireDuration;
	Info.bUseLargeFont = true;
	Info.bUseSuccessFailIcons = UseSuccessFailIcons;

	if (WidthOverride != 0.0f) {
		Info.WidthOverride = FOptionalSize(WidthOverride);
	}

	Info.Image = SlateBrush;

	SetNotificationSubText(Info, SubText);

	/* Call handler before adding notification */
	if (PreAddHandler) {
		PreAddHandler(Info);
	}

	/* Callers here want the item back to drive it later, so this one is added on the spot and
	 * hands back nothing when a blocking wait made that unsafe */
	return AddNotificationWhenSafe(Info, CompletionState);
}

/* Takes the handle by reference and empties it.
 *
 * Fadeout only starts the notification on its way out, so the item stays alive for a while after
 * this returns and the handle would still pin. Callers use theirs to tell "a notification is up"
 * from "none is", so leaving it pointing at a fading item reads as still up. */
inline void RemoveNotification(TWeakPtr<SNotificationItem>& Notification) {
	const TSharedPtr<SNotificationItem> Item = Notification.Pin();

	if (Item.IsValid()) {
		Item->SetFadeOutDuration(0.001f);
		Item->Fadeout();
	}

	Notification.Reset();
}
