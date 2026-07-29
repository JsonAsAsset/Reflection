/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Validation/Validation.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Engine/Compatibility.h"

/* Validation is UE5 only */
#if ENGINE_UE5

class SSearchBox;
class SEditableTextBox;

/* Column identifiers of the results list */
namespace ValidationColumns {
	const FName Status(TEXT("Status"));
	const FName Asset(TEXT("Asset"));
	const FName Type(TEXT("Type"));
	const FName PackagePath(TEXT("PackagePath"));
	const FName Location(TEXT("Location"));
}

/* Validates the project's content against the real game files and lists everything that
 * isn't where it should be. */
class REFLECTION_API SValidationPanel : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(SValidationPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SValidationPanel() override;

	virtual void Tick(const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime) override;

	/* Points validation at a content folder without running it, ex: "/Game/Characters" */
	void SetRootPath(const FString& RootPath);

	/* Runs validation over whatever folder is currently set */
	void StartValidation();

private:
	/* Toolbar ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	TSharedRef<SWidget> BuildToolbar();
	TSharedRef<SWidget> BuildFilterBar();
	TSharedRef<SWidget> BuildStatusFilter(EValidationStatus Status, const FText& Label);
	TSharedRef<SWidget> BuildFooter();

	FReply OnRunClicked();
	FReply OnCancelClicked();
	FReply OnCopyReportClicked();

	/* Points the folder box at whatever is selected in the Content Browser */
	FReply OnUseCurrentFolderClicked();

	bool CanRun() const;
	bool IsRunning() const;

	/* Whether the Content Browser has a folder selected to borrow */
	bool HasSelectedFolder() const;

	/* Names the folder that would be used, so the button says where it's pointing */
	FText GetUseCurrentFolderTooltip() const;

	/* Results ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	TSharedRef<ITableRow> GenerateRow(TSharedPtr<FValidationEntry> Entry, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedPtr<SWidget> OpenRowContextMenu();
	void OnRowDoubleClicked(TSharedPtr<FValidationEntry> Entry);
	void OnSelectionChanged(TSharedPtr<FValidationEntry> Entry, ESelectInfo::Type SelectInfo);

	EColumnSortMode::Type GetSortModeForColumn(FName ColumnId) const;
	void OnColumnSorted(EColumnSortPriority::Type Priority, const FName& ColumnId, EColumnSortMode::Type NewSortMode);

	/* Rebuilds the visible rows from the validation results, the search text and the status filters */
	void RefreshList();

	/* Marks the rows as needing a rebuild on the next Tick */
	void InvalidateList();

	/* Called by the validation whenever results change */
	void OnValidationUpdated();

	bool PassesFilters(const TSharedPtr<FValidationEntry>& Entry) const;

	/* Details ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	TSharedRef<SWidget> BuildDetails() const;
	static FText GetResolvedPathText(const TSharedPtr<FValidationEntry>& Entry);

	/* Actions ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	static void BrowseToEntry(TSharedPtr<FValidationEntry> Entry);
	static void CopyToClipboard(FString Text);
	static void MoveEntryTo(TSharedPtr<FValidationEntry> Entry, FString Destination);

	/* Text ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	FText GetSummaryText() const;
	FText GetProgressText() const;
	TOptional<float> GetProgress() const;
	EVisibility GetProgressVisibility() const;
	EVisibility GetFailureVisibility() const;
	FText GetFailureText() const;

	/* State ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	TSharedPtr<FValidation> Validation;
	FDelegateHandle ValidationUpdatedHandle;

	TArray<TSharedPtr<FValidationEntry>> VisibleEntries;

	TSharedPtr<SListView<TSharedPtr<FValidationEntry>>> ListView;
	TSharedPtr<SEditableTextBox> RootPathBox;
	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<SBox> DetailsContainer;

	FString SearchText;

	/* Only broken results are listable. Valid has no entry and no toggle, so PassesFilters
	 * drops it on the FindRef default, keeping the list to what actually needs attention. */
	TMap<EValidationStatus, bool> StatusFilters = {
		{ EValidationStatus::Misplaced, true },
		{ EValidationStatus::Missing, true }
	};

	FName SortColumn = ValidationColumns::PackagePath;
	EColumnSortMode::Type SortMode = EColumnSortMode::Ascending;

	/* Rebuilding the rows is O(n log n), so running validation only gets to do it a few times a second */
	static constexpr double RefreshInterval = 0.25;

	bool bListDirty = false;
	double LastRefreshTime = 0.0;
};

#endif
