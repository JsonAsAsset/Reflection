/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SEditableTextBox;
class SWindow;

/* What the dialog was shut with */
enum class EReflectFolderChoice : uint8 {
	/* Nothing to run: the window was closed, or nothing was found */
	Cancelled,

	/* Run what was listed */
	Reflect,

	/* Hand back to the window this one was opened from */
	Back
};

/* Picks a folder of game files to reflect, and shows what is in it before anything runs.
 *
 * Listing is a Cloud request rather than something the editor can answer, so it happens when
 * asked for instead of as the folder is typed. */
class REFLECTION_API SReflectFolderDialog : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(SReflectFolderDialog)
		: _UseClipboard(false)
		, _CanGoBack(false)
		{}
		SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
		SLATE_ARGUMENT(FString, InitialFolder)

		/* Whether an empty InitialFolder may fall back to whatever was copied. Only for the ways in
		 * that have no folder of their own: one that names a folder means that folder. */
		SLATE_ARGUMENT(bool, UseClipboard)

		/* Whether there is a window behind this one to go back to. Right-clicking a folder opens
		 * this one first, and back from the first window is just closing it. */
		SLATE_ARGUMENT(bool, CanGoBack)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* Runs the dialog modally. Reflect is the answer that fills OutPaths with what Cloud listed
	 * under the folder; the rest leave it alone. */
	static EReflectFolderChoice Open(TArray<FString>& OutPaths, TSet<FString>& OutAllowedTypes, const FString& InitialFolder = FString(), bool bUseClipboard = false, bool bCanGoBack = false);

private:
	FReply OnBackClicked();
	FReply OnFindClicked();
	FReply OnTypesClicked();
	FReply OnUseSelectedFolderClicked();
	FReply OnReflectClicked();

	void OnFolderCommitted(const FText& NewText, ETextCommit::Type CommitType);

	/* Asks Cloud what is under whatever the box currently holds */
	void Find();

	bool CanFind() const;
	bool CanReflect() const;

	/* Only there when there is a window behind this one */
	EVisibility GetBackVisibility() const;

	/* Whether the Content Browser has a folder selected to borrow */
	bool HasSelectedFolder() const;
	FText GetSelectedFolderTooltip() const;

	FText GetStatusText() const;
	FText GetReflectText() const;
	FText GetTypesText() const;

	TSharedRef<ITableRow> GenerateRow(TSharedPtr<FString> Path, const TSharedRef<STableViewBase>& OwnerTable) const;

	TSharedPtr<SWindow> ParentWindow;
	TSharedPtr<SEditableTextBox> FolderBox;
	TSharedPtr<SListView<TSharedPtr<FString>>> ListView;

	/* The last Find's results, as rows and in the form the importer takes */
	TArray<TSharedPtr<FString>> Rows;
	TArray<FString> Paths;

	/* Which types the run may build. Empty is no filter, which is what it starts as. */
	TSet<FString> AllowedTypes;

	/* Lets the status line tell "nothing looked for yet" apart from "nothing there" */
	bool Searched = false;

	bool CanGoBack = false;

	/* Which of the two buttons that close the window was pressed. Neither, and the window was shut
	 * some other way, which is neither answer. */
	bool Accepted = false;
	bool BackRequested = false;
};
