/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SEditableTextBox;
class SWindow;

/* Picks a folder of game files to reflect, and shows what is in it before anything runs.
 *
 * Listing is a Cloud request rather than something the editor can answer, so it happens when
 * asked for instead of as the folder is typed. */
class REFLECTION_API SReflectFolderDialog : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(SReflectFolderDialog) {}
		SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
		SLATE_ARGUMENT(FString, InitialFolder)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* Runs the dialog modally. True when the user chose to reflect, with OutPaths holding what
	 * Cloud listed under the folder. */
	static bool Open(TArray<FString>& OutPaths, const FString& InitialFolder = FString());

private:
	FReply OnFindClicked();
	FReply OnUseSelectedFolderClicked();
	FReply OnReflectClicked();

	void OnFolderCommitted(const FText& NewText, ETextCommit::Type CommitType);

	/* Asks Cloud what is under whatever the box currently holds */
	void Find();

	bool CanFind() const;
	bool CanReflect() const;

	/* Whether the Content Browser has a folder selected to borrow */
	bool HasSelectedFolder() const;
	FText GetSelectedFolderTooltip() const;

	FText GetStatusText() const;
	FText GetReflectText() const;

	TSharedRef<ITableRow> GenerateRow(TSharedPtr<FString> Path, const TSharedRef<STableViewBase>& OwnerTable) const;

	TSharedPtr<SWindow> ParentWindow;
	TSharedPtr<SEditableTextBox> FolderBox;
	TSharedPtr<SListView<TSharedPtr<FString>>> ListView;

	/* The last Find's results, as rows and in the form the importer takes */
	TArray<TSharedPtr<FString>> Rows;
	TArray<FString> Paths;

	/* Lets the status line tell "nothing looked for yet" apart from "nothing there" */
	bool Searched = false;

	bool Accepted = false;
};
