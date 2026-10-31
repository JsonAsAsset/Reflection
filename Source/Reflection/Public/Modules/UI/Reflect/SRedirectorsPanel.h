/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

/* The redirectors, as something to change rather than something to read.
 *
 * They live in the settings and have always been editable there, a page away from the paths they
 * act on. Which is where the trouble is: a redirect is only ever written for an asset somebody is
 * about to reflect, and whether it was written right cannot be seen from the page it was typed on.
 *
 * So they are here instead, beside the queue, and every change is answered straight away: each one
 * says how many of the queued paths it moves, and the queue says where each path now lands.
 *
 * The order they are in is part of what they do, since each works on what the one above it left,
 * so they are a list that can be dragged rather than a column that cannot. */
class SRedirectorsPanel : public SCompoundWidget {
public:
	DECLARE_DELEGATE(FOnRedirectorsChanged)

	/* Where the queued paths land before any redirect, which is what a redirector is counted
	 * against. Asked for rather than held, since the queue goes on being added to. */
	DECLARE_DELEGATE_RetVal(TArray<FString>, FOnGetLandingPaths)

	SLATE_BEGIN_ARGS(SRedirectorsPanel) {}
		/* Anything changed, saved by the time it fires */
		SLATE_EVENT(FOnRedirectorsChanged, OnChanged)

		SLATE_EVENT(FOnGetLandingPaths, LandingPaths)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* Builds the list again from the settings. Called for itself after a change, and from outside
	 * when the queue has moved on underneath it. */
	void Refresh();

private:
	/* One row per redirector, holding where it sits. Rebuilt whenever the settings change, so an
	 * index is only ever read back in the same breath it was handed out. */
	using FRowItem = TSharedPtr<int32>;

	TSharedRef<ITableRow> GenerateRow(FRowItem Item, const TSharedRef<STableViewBase>& OwnerTable);

	/* One redirector, and the rules under it */
	TSharedRef<SWidget> BuildCard(int32 Index);

	/* One From and To pair, which is the only part of a redirect that does anything */
	TSharedRef<SWidget> BuildRule(int32 Index, int32 At);

	/* What this one does to the queue, or why it is doing nothing to it */
	FText EffectText(int32 Index) const;

	/* Dragging one out of the order it is in, which is the order it is applied in */
	FReply OnRowDragDetected(const FGeometry& Geometry, const FPointerEvent& MouseEvent, FRowItem Item);
	TOptional<EItemDropZone> OnRowCanAcceptDrop(const FDragDropEvent& DragDropEvent, EItemDropZone Zone, FRowItem Item);
	FReply OnRowAcceptDrop(const FDragDropEvent& DragDropEvent, EItemDropZone Zone, FRowItem Item);

	FReply OnAddRedirectorClicked();
	FReply OnRemoveRedirectorClicked(int32 Index);
	FReply OnAddRuleClicked(int32 Index);
	FReply OnRemoveRuleClicked(int32 Index, int32 At);

	/* Written to the settings and reported on, which is the same two steps for every edit here */
	void Commit();

	EVisibility GetEmptyVisibility() const;

	FOnRedirectorsChanged OnChanged;
	FOnGetLandingPaths LandingPaths;

	TArray<FRowItem> Items;
	TSharedPtr<SListView<FRowItem>> List;
};
