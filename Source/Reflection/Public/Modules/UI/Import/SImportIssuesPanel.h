/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

/* One line of the tree: an asset, or one of its issues */
struct FImportIssueRow {
	TSharedPtr<FImportIssueAsset> Asset;
	TSharedPtr<FImportIssue> Issue;

	TArray<TSharedPtr<FImportIssueRow>> Children;

	bool IsAsset() const {
		return !Issue.IsValid();
	}
};

/* What an import run couldn't do, one expandable row per asset */
class REFLECTION_API SImportIssuesPanel : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(SImportIssuesPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* Brings up the window over whatever FImportIssues collected, reusing it if it's already up */
	static void Open();

	/* Rebuilds the rows from the current issues */
	void Refresh();

private:
	TSharedRef<SWidget> BuildHeader();

	TSharedRef<ITableRow> GenerateRow(TSharedPtr<FImportIssueRow> Row, const TSharedRef<STableViewBase>& OwnerTable);
	void GetRowChildren(TSharedPtr<FImportIssueRow> Row, TArray<TSharedPtr<FImportIssueRow>>& OutChildren);
	void OnRowDoubleClicked(TSharedPtr<FImportIssueRow> Row);

	FReply OnExpandAllClicked();
	FReply OnCollapseAllClicked();
	FReply OnCopyReportClicked();

	FText GetSummaryText() const;

	/* Every issue as text, the thing that ends up in a bug report */
	FString BuildReport() const;

	TSharedPtr<STreeView<TSharedPtr<FImportIssueRow>>> TreeView;

	TArray<TSharedPtr<FImportIssueRow>> Rows;
};
