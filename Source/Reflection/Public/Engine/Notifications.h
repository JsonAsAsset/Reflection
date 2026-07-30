/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#include "Editor.h"
#include "TimerManager.h"
#include "Modules/Cloud/Remote.h"

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
		GEditor->GetTimerManager()->SetTimerForNextTick(FTimerDelegate::CreateLambda([Info, CompletionState] {
			if (const TSharedPtr<SNotificationItem> DeferredItem = FSlateNotificationManager::Get().AddNotification(Info)) {
				DeferredItem->SetCompletionState(CompletionState);
			}
		}));

		return nullptr;
	}

	const TSharedPtr<SNotificationItem> NotificationPtr = FSlateNotificationManager::Get().AddNotification(Info);

	if (NotificationPtr.IsValid()) {
		NotificationPtr->SetCompletionState(CompletionState);
	}

	return NotificationPtr;
}

/* Show the user a Notification */
inline auto AppendNotification(const FText& Text, const FText& SubText, const float ExpireDuration,
                               const SNotificationItem::ECompletionState CompletionState, const bool UseSuccessFailIcons,
                               const float WidthOverride) -> void
{
	FNotificationInfo Info = FNotificationInfo(Text);
	Info.ExpireDuration = ExpireDuration;
	Info.bUseLargeFont = true;
	Info.bUseSuccessFailIcons = UseSuccessFailIcons;
	Info.WidthOverride = FOptionalSize(WidthOverride);

	SetNotificationSubText(Info, SubText);

	AddNotificationWhenSafe(Info, CompletionState);
}

/* Show the user a Notification with Subtext */
inline auto AppendNotification(const FText& Text, const FText& SubText, float ExpireDuration,
                               const FSlateBrush* SlateBrush, SNotificationItem::ECompletionState CompletionState,
                               const bool UseSuccessFailIcons, const float WidthOverride) -> void
{
	FNotificationInfo Info = FNotificationInfo(Text);
	Info.ExpireDuration = ExpireDuration;
	Info.bUseLargeFont = true;
	Info.bUseSuccessFailIcons = UseSuccessFailIcons;
	Info.WidthOverride = FOptionalSize(WidthOverride);
	Info.Image = SlateBrush;

	SetNotificationSubText(Info, SubText);

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
		Item->SetFadeOutDuration(0.001);
		Item->Fadeout();
	}

	Notification.Reset();
}
