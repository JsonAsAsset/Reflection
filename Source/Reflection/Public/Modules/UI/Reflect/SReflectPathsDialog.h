/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SEditableTextBox;
class SWindow;

/* What the dialog was shut with */
enum class EReflectPathsChoice : uint8 {
	/* Nothing to run: the window was closed, or the queue was empty */
	Cancelled,

	/* Run what is queued */
	Reflect,

	/* Reflect a folder instead. The queue goes with the window, which is what makes this a
	 * different answer rather than a second one. */
	Folder
};

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

	/* Runs the dialog modally. Reflect is the answer that fills OutPaths with everything queued,
	 * Folder the one that fills OutFolder with whatever the box was left holding; the rest leave
	 * both alone. */
	static EReflectPathsChoice Open(TArray<FString>& OutPaths, FString& OutFolder);

private:
	FReply OnAddClicked();
	FReply OnFolderClicked();
	FReply OnReflectClicked();

	void OnPathCommitted(const FText& NewText, ETextCommit::Type CommitType);

	/* Queues whatever is in the box, which may be several paths pasted at once */
	void AddFromBox();

	/* One entry per path, ignoring blanks and anything already queued */
	void AddPaths(const FString& Text);

	/* Takes one entry back out, from the cross on its own row */
	FReply RemovePath(TSharedPtr<FString> Path);

	bool CanAdd() const;
	bool CanReflect() const;

	FText GetStatusText() const;
	FText GetReflectText() const;

	TSharedRef<ITableRow> GenerateRow(TSharedPtr<FString> Path, const TSharedRef<STableViewBase>& OwnerTable);

	TSharedPtr<SWindow> ParentWindow;
	TSharedPtr<SEditableTextBox> PathBox;
	TSharedPtr<SListView<TSharedPtr<FString>>> ListView;

	TArray<TSharedPtr<FString>> Rows;

	/* Which of the two buttons that close the window was pressed. Neither, and the window was shut
	 * some other way, which is neither answer. */
	bool Accepted = false;
	bool FolderRequested = false;
};
