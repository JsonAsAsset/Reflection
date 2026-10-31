/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Cloud/Cloud.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

/* Which of Mirror's profiles is being read, and the way to a different one.
 *
 * A profile is one game's files and the mappings for them, and everything a reflect does is read
 * out of one. Which one was Mirror's to decide: whatever its own window had open. So changing it
 * meant leaving the editor, and what the reflect window said about paths was worked out against
 * whichever profile was open before.
 *
 * Two different things can be done about that here. A profile can be reflected from without Mirror
 * moving at all, which is what loading one and pointing the editor at it does: Mirror can serve any
 * profile it has mounted, and the editor says which in every request it sends. Or Mirror can be
 * asked to open one, which moves its window too, and is the one to use when the editor should
 * follow Mirror rather than the other way round.
 *
 * Nothing here parks the editor. The window this lives in is modal, and a modal loop ticks Slate
 * and nothing else: no engine tick, so no HTTP tick, so an answer asked for from inside one never
 * arrives on its own. A timer drives the requests instead, which is what lets the waiting happen
 * in the background rather than behind a progress dialog. */
class SProfilesPanel : public SCompoundWidget {
public:
	DECLARE_DELEGATE(FOnProfileChanged)

	SLATE_BEGIN_ARGS(SProfilesPanel) {}
		/* A different profile is being read, so anything worked out against the last one is stale */
		SLATE_EVENT(FOnProfileChanged, OnChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* Asks Mirror what it has. Called for itself after a change, and from outside when the page
	 * comes up, since Mirror may have been pointed somewhere else in the meantime. */
	void Refresh();

private:
	using FRowItem = TSharedPtr<Cloud::FBuild>;

	TSharedRef<ITableRow> GenerateRow(FRowItem Item, const TSharedRef<STableViewBase>& OwnerTable);

	/* Whether this is the one the editor reads out of, which is the pinned one where there is one
	 * and whatever Mirror has open where there is not */
	bool IsReadingFrom(const Cloud::FBuild& Build) const;

	/* The profile being read, said as its name alone, and what is worth knowing about it under
	 * that. Both answer for the cases where there is no profile to name. */
	FText GetHeadlineText() const;
	FText GetDetailText() const;

	/* Said over the list where there is no list: Mirror shut, or Mirror with nothing in it */
	FText GetEmptyText() const;

	EVisibility GetRunningVisibility() const;
	EVisibility GetNotRunningVisibility() const;
	EVisibility GetEmptyVisibility() const;

	/* Only there while the editor is pinned to a profile Mirror has not got open */
	EVisibility GetPinnedVisibility() const;

	FReply OnRefreshClicked();
	FReply OnSetupClicked();
	FReply OnFollowMirrorClicked();

	/* Reads out of this one from now on, mounting it first where Mirror has let it go. Mirror
	 * stays on whatever it had open. */
	FReply OnReadFromClicked(FRowItem Item);

	/* Asks Mirror to open it, which moves its window as well as what the editor reads */
	FReply OnOpenInMirrorClicked(FRowItem Item);

	/* Lets a mounted one go again */
	FReply OnUnloadClicked(FRowItem Item);

	/* Pins the editor to Id, tells whoever is holding paths, and says so */
	void ReadFrom(const FString& Id, const FString& Named);

	/* Drives the requests this page has outstanding, since nothing else is driving them while a
	 * modal window is up, and asks after a profile Mirror is still opening */
	EActiveTimerReturnType Pump(double CurrentTime, float DeltaTime);

	/* Starts the timer if it is not already going */
	void BeginWaiting(const FText& What, const FString& Id);

	/* Nothing outstanding, so the buttons come back and the timer stops */
	void FinishWaiting();

	bool IsIdle() const { return !Working; }

	/* What a row says in place of its buttons while it is the one being waited on */
	FText GetBusyText() const { return BusyWhat; }

	FOnProfileChanged OnChanged;

	TArray<FRowItem> Items;
	TSharedPtr<SListView<FRowItem>> List;

	/* Something was asked of Mirror and has not finished. One at a time: these change what the
	 * editor reads out from under itself, and two at once would race. */
	bool Working = false;

	/* The profile that is waiting on an answer, and what it is waiting for. Empty where the wait
	 * is not about one of them, which is what listing is. */
	FString BusyId;
	FText BusyWhat;

	/* A profile Mirror is opening, which it does on its own time and says nothing about, so the
	 * only way to know is to keep asking what it has open */
	FString Awaiting;

	double AskAgainAt = 0.0;
	double GiveUpOpeningAt = 0.0;
	bool AskingMetadata = false;

	TSharedPtr<FActiveTimerHandle> Waiting;

	/* Whether Mirror has been asked yet, so "nothing to show" can be told from "not asked" */
	bool Asked = false;
};
