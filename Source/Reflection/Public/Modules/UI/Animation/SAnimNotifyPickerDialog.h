/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SWindow;

/* One notify class the game's copy of a sequence carries, and whether it is wanted. */
struct FAnimNotifyClassEntry {
	/* The class as the export names it, which is what tells one notify from another */
	FString Class;

	/* What the sequence calls them, which is the name the editor draws on the track */
	FString Named;

	int32 Count = 0;

	bool Chosen = false;
};

/* Picks which of a sequence's notifies to bring over.
 *
 * By class rather than one at a time: a notify is only interesting for what it does, and a
 * sequence carries the same handful of them over and over at different times. Nothing is ticked
 * to start with, since this runs on an animation somebody already has and nothing should land on
 * it that was not asked for. */
class REFLECTION_API SAnimNotifyPickerDialog : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(SAnimNotifyPickerDialog) {}
		SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
		SLATE_ARGUMENT(FString, Sequence)
		SLATE_ARGUMENT(TArray<FAnimNotifyClassEntry>, Entries)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* Runs modally. True when the reader accepted, with InOutEntries saying what stayed ticked. */
	static bool Open(const FString& Sequence, TArray<FAnimNotifyClassEntry>& InOutEntries);

private:
	using FRowItem = TSharedPtr<FAnimNotifyClassEntry>;

	TSharedRef<ITableRow> GenerateRow(FRowItem Item, const TSharedRef<STableViewBase>& OwnerTable);

	FReply OnAllClicked();
	FReply OnNoneClicked();
	FReply OnAcceptClicked();

	bool CanAccept() const;

	FText GetSummaryText() const;
	FText GetAcceptText() const;

	int32 CountChosen() const;

	TArray<FRowItem> Entries;

	TSharedPtr<SListView<FRowItem>> ListView;
	TSharedPtr<SWindow> ParentWindow;

	FString Sequence;

	bool Accepted = false;
};
