/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SEditableTextBox;
class SWindow;

/* Builds up a list of asset paths to reflect, one Cloud knows about rather than one in this
 * project.
 *
 * Nothing here can be checked without asking Cloud for the asset itself, so the dialog is about
 * getting the list right: what is queued stays visible, and anything wrong can be taken back out
 * before the run starts. */
class REFLECTION_API SReflectPathsDialog : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(SReflectPathsDialog) {}
		SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* Runs the dialog modally. True when the user chose to reflect, with OutPaths holding
	 * everything queued. */
	static bool Open(TArray<FString>& OutPaths);

private:
	FReply OnAddClicked();
	FReply OnRemoveClicked();
	FReply OnReflectClicked();
	FReply OnCancelClicked();

	void OnPathCommitted(const FText& NewText, ETextCommit::Type CommitType);

	/* Queues whatever is in the box, which may be several paths pasted at once */
	void AddFromBox();

	/* One entry per path, ignoring blanks and anything already queued */
	void AddPaths(const FString& Text);

	bool CanAdd() const;
	bool CanRemove() const;
	bool CanReflect() const;

	FText GetStatusText() const;
	FText GetReflectText() const;

	TSharedRef<ITableRow> GenerateRow(TSharedPtr<FString> Path, const TSharedRef<STableViewBase>& OwnerTable) const;

	TSharedPtr<SWindow> ParentWindow;
	TSharedPtr<SEditableTextBox> PathBox;
	TSharedPtr<SListView<TSharedPtr<FString>>> ListView;

	TArray<TSharedPtr<FString>> Rows;

	bool Accepted = false;
};
