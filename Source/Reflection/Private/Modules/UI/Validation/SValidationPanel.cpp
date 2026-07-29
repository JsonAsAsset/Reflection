/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/Validation/SValidationPanel.h"

/* Validation is UE5 only */
#if ENGINE_UE5

#include "AssetToolsModule.h"
#include "IAssetTools.h"

#include "Engine/Compatibility.h"
#include "Engine/EngineUtilities.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"

#include "Modules/Cloud/Cloud.h"
#include "Modules/Log.h"

#include "Styling/CoreStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"

#define LOCTEXT_NAMESPACE "Reflection.Validation"

/* Row of the results list ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

class SValidationRow : public SMultiColumnTableRow<TSharedPtr<FValidationEntry>> {
public:
	SLATE_BEGIN_ARGS(SValidationRow) {}
		SLATE_ARGUMENT(TSharedPtr<FValidationEntry>, Entry)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable) {
		Entry = InArgs._Entry;

		SMultiColumnTableRow::Construct(FSuperRowType::FArguments(), OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnId) override {
		if (!Entry.IsValid()) {
			return SNullWidget::NullWidget;
		}

		if (ColumnId == ValidationColumns::Status) {
			return SNew(SBox)
				.Padding(FMargin(8.0f, 2.0f))
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(GetValidationStatusText(Entry->Status))
					.ColorAndOpacity(FSlateColor(GetValidationStatusColor(Entry->Status)))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				];
		}

		if (ColumnId == ValidationColumns::Asset) {
			return MakeTextColumn(FText::FromString(Entry->AssetName), FText::FromString(Entry->PackagePath));
		}

		if (ColumnId == ValidationColumns::Type) {
			return MakeTextColumn(FText::FromString(Entry->ClassName), FText::GetEmpty());
		}

		if (ColumnId == ValidationColumns::PackagePath) {
			return MakeTextColumn(FText::FromString(Entry->PackagePath), FText::FromString(Entry->ResolvedPath));
		}

		if (ColumnId == ValidationColumns::Location) {
			if (Entry->Status == EValidationStatus::Valid) {
				return MakeTextColumn(LOCTEXT("LocationMatches", "Same as project"), FText::GetEmpty());
			}

			if (Entry->Suggestions.Num() == 0) {
				return MakeTextColumn(LOCTEXT("LocationNowhere", "Not in the game"), FText::GetEmpty());
			}

			const FText Extra = Entry->Suggestions.Num() > 1
				? FText::Format(LOCTEXT("LocationMoreFmt", "{0} more location(s)"), FText::AsNumber(Entry->Suggestions.Num() - 1))
				: FText::GetEmpty();

			return MakeTextColumn(FText::FromString(Entry->GetSuggestedPath()), Extra);
		}

		return SNullWidget::NullWidget;
	}

private:
	static TSharedRef<SWidget> MakeTextColumn(const FText& Text, const FText& Tooltip) {
		return SNew(SBox)
			.Padding(FMargin(8.0f, 2.0f))
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Text)
				.ToolTipText(Tooltip.IsEmpty() ? Text : Tooltip)
			];
	}

	TSharedPtr<FValidationEntry> Entry;
};

/* Panel ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

void SValidationPanel::Construct(const FArguments& InArgs) {
	Validation = MakeShared<FValidation>();
	ValidationUpdatedHandle = Validation->OnUpdated.AddSP(this, &SValidationPanel::OnValidationUpdated);

	ChildSlot
	[
		SNew(SVerticalBox)

		/* Toolbar */
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(8.0f, 6.0f))
			[
				BuildToolbar()
			]
		]

		/* Filters */
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 4.0f))
		[
			BuildFilterBar()
		]

		/* Reason validation couldn't run */
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 0.0f, 8.0f, 4.0f))
		[
			SNew(SBorder)
			.Visibility(this, &SValidationPanel::GetFailureVisibility)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(8.0f, 6.0f))
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor(FLinearColor(0.95f, 0.66f, 0.20f)))
				.Text(this, &SValidationPanel::GetFailureText)
			]
		]

		/* Results */
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(FMargin(8.0f, 0.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(1.0f))
			[
				SAssignNew(ListView, SListView<TSharedPtr<FValidationEntry>>)
				.ListItemsSource(&VisibleEntries)
				.SelectionMode(ESelectionMode::Multi)
				.OnGenerateRow(this, &SValidationPanel::GenerateRow)
				.OnContextMenuOpening(this, &SValidationPanel::OpenRowContextMenu)
				.OnMouseButtonDoubleClick(this, &SValidationPanel::OnRowDoubleClicked)
				.OnSelectionChanged(this, &SValidationPanel::OnSelectionChanged)
				.HeaderRow(
					SNew(SHeaderRow)

					+ SHeaderRow::Column(ValidationColumns::Status)
					.DefaultLabel(LOCTEXT("ColumnStatus", "Status"))
					.FillWidth(0.10f)
					.SortMode(this, &SValidationPanel::GetSortModeForColumn, ValidationColumns::Status)
					.OnSort(this, &SValidationPanel::OnColumnSorted)

					+ SHeaderRow::Column(ValidationColumns::Asset)
					.DefaultLabel(LOCTEXT("ColumnAsset", "Asset"))
					.FillWidth(0.20f)
					.SortMode(this, &SValidationPanel::GetSortModeForColumn, ValidationColumns::Asset)
					.OnSort(this, &SValidationPanel::OnColumnSorted)

					+ SHeaderRow::Column(ValidationColumns::Type)
					.DefaultLabel(LOCTEXT("ColumnType", "Type"))
					.FillWidth(0.13f)
					.SortMode(this, &SValidationPanel::GetSortModeForColumn, ValidationColumns::Type)
					.OnSort(this, &SValidationPanel::OnColumnSorted)

					+ SHeaderRow::Column(ValidationColumns::PackagePath)
					.DefaultLabel(LOCTEXT("ColumnPackagePath", "Path In Project"))
					.FillWidth(0.30f)
					.SortMode(this, &SValidationPanel::GetSortModeForColumn, ValidationColumns::PackagePath)
					.OnSort(this, &SValidationPanel::OnColumnSorted)

					+ SHeaderRow::Column(ValidationColumns::Location)
					.DefaultLabel(LOCTEXT("ColumnLocation", "Path In Game"))
					.FillWidth(0.27f)
					.SortMode(this, &SValidationPanel::GetSortModeForColumn, ValidationColumns::Location)
					.OnSort(this, &SValidationPanel::OnColumnSorted)
				)
			]
		]

		/* Details of the selected row */
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 4.0f))
		[
			SAssignNew(DetailsContainer, SBox)
			[
				BuildDetails()
			]
		]

		/* Footer */
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(8.0f, 6.0f))
			[
				BuildFooter()
			]
		]
	];
}

SValidationPanel::~SValidationPanel() {
	if (Validation.IsValid()) {
		Validation->OnUpdated.Remove(ValidationUpdatedHandle);
		Validation->Cancel();
	}
}

void SValidationPanel::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) {
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	if (!bListDirty) {
		return;
	}

	/* Batches land faster than the list is worth rebuilding, but once validation stops the
	 * results have to be up to date immediately */
	if (IsRunning() && InCurrentTime - LastRefreshTime < RefreshInterval) {
		return;
	}

	LastRefreshTime = InCurrentTime;
	bListDirty = false;

	RefreshList();

	if (DetailsContainer.IsValid()) {
		DetailsContainer->SetContent(BuildDetails());
	}
}

TSharedRef<SWidget> SValidationPanel::BuildToolbar() {
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
		[
			SNew(SButton)
			.Text(LOCTEXT("RunValidation", "Run Validation"))
			.ToolTipText(LOCTEXT("RunValidationTooltip", "Check every asset under the folder below against the real game files."))
			.IsEnabled(this, &SValidationPanel::CanRun)
			.OnClicked(this, &SValidationPanel::OnRunClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, 16.0f, 0.0f))
		[
			SNew(SButton)
			.Text(LOCTEXT("CancelValidation", "Cancel"))
			.ToolTipText(LOCTEXT("CancelValidationTooltip", "Stop the running validation and keep the results gathered so far."))
			.IsEnabled(this, &SValidationPanel::IsRunning)
			.OnClicked(this, &SValidationPanel::OnCancelClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("RootLabel", "Folder"))
		]

		+ SHorizontalBox::Slot()
		.FillWidth(0.4f)
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
		[
			SAssignNew(RootPathBox, SEditableTextBox)
			.Text(FText::FromString(TEXT("/Game")))
			.ToolTipText(LOCTEXT("RootTooltip", "Content folder to validate, ex: /Game or /Game/Characters."))
			.HintText(LOCTEXT("RootHint", "/Game"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, 16.0f, 0.0f))
		[
			SNew(SButton)
			.Text(LOCTEXT("UseCurrentFolder", "Use Current"))
			.ToolTipText(this, &SValidationPanel::GetUseCurrentFolderTooltip)
			.IsEnabled(this, &SValidationPanel::HasSelectedFolder)
			.OnClicked(this, &SValidationPanel::OnUseCurrentFolderClicked)
		]

		+ SHorizontalBox::Slot()
		.FillWidth(0.6f)
		.VAlign(VAlign_Center)
		[
			SAssignNew(SearchBox, SSearchBox)
			.HintText(LOCTEXT("SearchHint", "Filter results"))
			.OnTextChanged_Lambda([this](const FText& NewText) {
				SearchText = NewText.ToString();
				RefreshList();
			})
		];
}

TSharedRef<SWidget> SValidationPanel::BuildFilterBar() {
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ShowLabel", "Show"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
		[
			BuildStatusFilter(EValidationStatus::Misplaced, LOCTEXT("FilterMisplaced", "Misplaced"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
		[
			BuildStatusFilter(EValidationStatus::Missing, LOCTEXT("FilterMissing", "Missing"))
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.Text(LOCTEXT("CopyReport", "Copy Report"))
			.ToolTipText(LOCTEXT("CopyReportTooltip", "Copy the listed results to the clipboard as tab separated text."))
			.OnClicked(this, &SValidationPanel::OnCopyReportClicked)
		];
}

TSharedRef<SWidget> SValidationPanel::BuildStatusFilter(const EValidationStatus Status, const FText& Label) {
	return SNew(SCheckBox)
		.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
		.Padding(FMargin(8.0f, 2.0f))
		.IsChecked_Lambda([this, Status] {
			return StatusFilters.FindRef(Status) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, Status](const ECheckBoxState NewState) {
			StatusFilters.Add(Status, NewState == ECheckBoxState::Checked);
			RefreshList();
		})
		[
			SNew(STextBlock)
			.Text_Lambda([this, Status, Label] {
				return FText::Format(
					LOCTEXT("FilterCountFmt", "{0} ({1})"),
					Label,
					FText::AsNumber(Validation.IsValid() ? Validation->GetCount(Status) : 0)
				);
			})
			.ColorAndOpacity(FSlateColor(GetValidationStatusColor(Status)))
		];
}

TSharedRef<SWidget> SValidationPanel::BuildFooter() {
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(this, &SValidationPanel::GetSummaryText)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(8.0f, 0.0f))
		[
			SNew(STextBlock)
			.Visibility(this, &SValidationPanel::GetProgressVisibility)
			.Text(this, &SValidationPanel::GetProgressText)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(160.0f)
			.HeightOverride(10.0f)
			.Visibility(this, &SValidationPanel::GetProgressVisibility)
			[
				SNew(SProgressBar)
				.Percent(this, &SValidationPanel::GetProgress)
			]
		];
}

/* Running ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

/* Turns whatever the user typed into a package path the Asset Registry will accept */
static FString NormalizeRootPath(const FString& InRootPath) {
	FString RootPath = InRootPath.TrimStartAndEnd();

	if (!RootPath.StartsWith(TEXT("/"))) {
		RootPath = TEXT("/") + RootPath;
	}

	RootPath.RemoveFromEnd(TEXT("/"));

	return RootPath.IsEmpty() || RootPath == TEXT("/") ? TEXT("/Game") : RootPath;
}

void SValidationPanel::SetRootPath(const FString& RootPath) {
	if (RootPathBox.IsValid()) {
		RootPathBox->SetText(FText::FromString(NormalizeRootPath(RootPath)));
	}
}

void SValidationPanel::StartValidation() {
	if (!Validation.IsValid() || !RootPathBox.IsValid()) {
		return;
	}

	const FString RootPath = NormalizeRootPath(RootPathBox->GetText().ToString());
	RootPathBox->SetText(FText::FromString(RootPath));

	TWeakPtr<SValidationPanel> WeakPanel = SharedThis(this);

	/* Surfaces the usual "no Cloud instance" notification when Core isn't up */
	Cloud::Status::Check(GetSettings(), [WeakPanel, RootPath](const bool bReady) {
		const TSharedPtr<SValidationPanel> Panel = WeakPanel.Pin();
		if (!Panel.IsValid() || !Panel->Validation.IsValid()) {
			return;
		}

		if (!bReady) {
			Panel->Validation->Cancel();

			return;
		}

		Panel->Validation->Start(RootPath);
	});
}

FReply SValidationPanel::OnRunClicked() {
	StartValidation();

	return FReply::Handled();
}

FReply SValidationPanel::OnCancelClicked() {
	if (Validation.IsValid()) {
		Validation->Cancel();
		OnValidationUpdated();
	}

	return FReply::Handled();
}

FReply SValidationPanel::OnCopyReportClicked() {
	FString Report = TEXT("Status\tAsset\tType\tPath In Project\tPath In Game\n");

	for (const TSharedPtr<FValidationEntry>& Entry : VisibleEntries) {
		Report += FString::Printf(
			TEXT("%s\t%s\t%s\t%s\t%s\n"),
			*GetValidationStatusText(Entry->Status).ToString(),
			*Entry->AssetName,
			*Entry->ClassName,
			*Entry->PackagePath,
			*Entry->GetSuggestedPath()
		);
	}

	CopyToClipboard(Report);

	return FReply::Handled();
}

FReply SValidationPanel::OnUseCurrentFolderClicked() {
	const FString SelectedFolder = GetSelectedContentBrowserFolder();

	if (!SelectedFolder.IsEmpty()) {
		SetRootPath(SelectedFolder);
	}

	return FReply::Handled();
}

bool SValidationPanel::HasSelectedFolder() const {
	return !GetSelectedContentBrowserFolder().IsEmpty();
}

FText SValidationPanel::GetUseCurrentFolderTooltip() const {
	const FString SelectedFolder = GetSelectedContentBrowserFolder();

	if (SelectedFolder.IsEmpty()) {
		return LOCTEXT("UseCurrentFolderNone", "Select a folder in the Content Browser to use it here.");
	}

	return FText::Format(
		LOCTEXT("UseCurrentFolderFmt", "Use the folder selected in the Content Browser ({0})."),
		FText::FromString(SelectedFolder)
	);
}

bool SValidationPanel::CanRun() const {
	return Validation.IsValid() && !Validation->IsRunning();
}

bool SValidationPanel::IsRunning() const {
	return Validation.IsValid() && Validation->IsRunning();
}

/* Results ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

TSharedRef<ITableRow> SValidationPanel::GenerateRow(TSharedPtr<FValidationEntry> Entry, const TSharedRef<STableViewBase>& OwnerTable) {
	return SNew(SValidationRow, OwnerTable)
		.Entry(Entry);
}

TSharedPtr<SWidget> SValidationPanel::OpenRowContextMenu() {
	if (!ListView.IsValid()) {
		return nullptr;
	}

	TArray<TSharedPtr<FValidationEntry>> Selection = ListView->GetSelectedItems();
	if (Selection.Num() == 0) {
		return nullptr;
	}

	const TSharedPtr<FValidationEntry> Entry = Selection[0];

	FMenuBuilder MenuBuilder(true, nullptr);

	MenuBuilder.BeginSection("ReflectionValidationAsset", LOCTEXT("MenuAssetSection", "Asset"));
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("MenuBrowseTo", "Browse To Asset"),
			LOCTEXT("MenuBrowseToTooltip", "Select this asset in the Content Browser."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&SValidationPanel::BrowseToEntry, Entry))
		);

		MenuBuilder.AddMenuEntry(
			LOCTEXT("MenuCopyPath", "Copy Path In Project"),
			LOCTEXT("MenuCopyPathTooltip", "Copy the package path this asset currently sits at."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateStatic(&SValidationPanel::CopyToClipboard, Entry->PackagePath))
		);
	}
	MenuBuilder.EndSection();

	if (Entry->Suggestions.Num() > 0) {
		MenuBuilder.BeginSection("ReflectionValidationLocations", LOCTEXT("MenuLocationsSection", "Real Locations"));
		{
			for (const FString& Suggestion : Entry->Suggestions) {
				FString Folder;
				if (!Suggestion.Split(TEXT("/"), &Folder, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
					continue;
				}

				MenuBuilder.AddSubMenu(
					FText::FromString(Suggestion),
					LOCTEXT("MenuLocationTooltip", "This asset name exists here in the game."),
					FNewMenuDelegate::CreateLambda([Entry, Suggestion, Folder](FMenuBuilder& SubMenuBuilder) {
						SubMenuBuilder.AddMenuEntry(
							LOCTEXT("MenuCopyLocation", "Copy Path"),
							LOCTEXT("MenuCopyLocationTooltip", "Copy this path to the clipboard."),
							FSlateIcon(),
							FUIAction(FExecuteAction::CreateStatic(&SValidationPanel::CopyToClipboard, Suggestion))
						);

						SubMenuBuilder.AddMenuEntry(
							LOCTEXT("MenuMoveHere", "Move Asset Here"),
							LOCTEXT("MenuMoveHereTooltip", "Move the project's asset into this folder, leaving a redirector behind."),
							FSlateIcon(),
							FUIAction(FExecuteAction::CreateStatic(&SValidationPanel::MoveEntryTo, Entry, Folder))
						);
					})
				);
			}
		}
		MenuBuilder.EndSection();
	}

	return MenuBuilder.MakeWidget();
}

void SValidationPanel::OnRowDoubleClicked(TSharedPtr<FValidationEntry> Entry) {
	BrowseToEntry(Entry);
}

void SValidationPanel::OnSelectionChanged(TSharedPtr<FValidationEntry> Entry, ESelectInfo::Type SelectInfo) {
	if (DetailsContainer.IsValid()) {
		DetailsContainer->SetContent(BuildDetails());
	}
}

EColumnSortMode::Type SValidationPanel::GetSortModeForColumn(const FName ColumnId) const {
	return SortColumn == ColumnId ? SortMode : EColumnSortMode::None;
}

void SValidationPanel::OnColumnSorted(EColumnSortPriority::Type Priority, const FName& ColumnId, EColumnSortMode::Type NewSortMode) {
	SortColumn = ColumnId;
	SortMode = NewSortMode;

	RefreshList();
}

void SValidationPanel::RefreshList() {
	VisibleEntries.Reset();

	if (Validation.IsValid()) {
		for (const TSharedPtr<FValidationEntry>& Entry : Validation->GetEntries()) {
			if (PassesFilters(Entry)) {
				VisibleEntries.Add(Entry);
			}
		}
	}

	const FName Column = SortColumn;
	const bool bAscending = SortMode != EColumnSortMode::Descending;

	VisibleEntries.Sort([Column, bAscending](const TSharedPtr<FValidationEntry>& A, const TSharedPtr<FValidationEntry>& B) {
		int32 Compare = 0;

		if (Column == ValidationColumns::Status) {
			Compare = static_cast<int32>(A->Status) - static_cast<int32>(B->Status);
		} else if (Column == ValidationColumns::Asset) {
			Compare = A->AssetName.Compare(B->AssetName, ESearchCase::IgnoreCase);
		} else if (Column == ValidationColumns::Type) {
			Compare = A->ClassName.Compare(B->ClassName, ESearchCase::IgnoreCase);
		} else if (Column == ValidationColumns::Location) {
			Compare = A->GetSuggestedPath().Compare(B->GetSuggestedPath(), ESearchCase::IgnoreCase);
		}

		/* Package paths are unique, so they settle any tie deterministically */
		if (Compare == 0) {
			Compare = A->PackagePath.Compare(B->PackagePath, ESearchCase::IgnoreCase);
		}

		return bAscending ? Compare < 0 : Compare > 0;
	});

	if (ListView.IsValid()) {
		ListView->RequestListRefresh();
	}
}

void SValidationPanel::InvalidateList() {
	bListDirty = true;
}

void SValidationPanel::OnValidationUpdated() {
	InvalidateList();
}

bool SValidationPanel::PassesFilters(const TSharedPtr<FValidationEntry>& Entry) const {
	if (!Entry.IsValid() || Entry->Status == EValidationStatus::Pending) {
		return false;
	}

	if (!StatusFilters.FindRef(Entry->Status)) {
		return false;
	}

	if (SearchText.IsEmpty()) {
		return true;
	}

	return Entry->PackagePath.Contains(SearchText)
		|| Entry->ClassName.Contains(SearchText)
		|| Entry->GetSuggestedPath().Contains(SearchText);
}

/* Details ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

/* Describes where the Cloud ended up looking. A hit and a miss want different wording, or a
 * valid asset reads like a failed lookup. */
FText SValidationPanel::GetResolvedPathText(const TSharedPtr<FValidationEntry>& Entry) {
	if (Entry->ResolvedPath.IsEmpty()) {
		return LOCTEXT("DetailsUnresolvable", "This path couldn't be resolved to a game file.");
	}

	if (Entry->Status == EValidationStatus::Valid) {
		return FText::Format(
			LOCTEXT("DetailsFoundAtFmt", "Found in the game at {0}"),
			FText::FromString(Entry->ResolvedPath)
		);
	}

	return FText::Format(
		LOCTEXT("DetailsLookedInFmt", "The Cloud looked for this at {0}"),
		FText::FromString(Entry->ResolvedPath)
	);
}

TSharedRef<SWidget> SValidationPanel::BuildDetails() const {
	TArray<TSharedPtr<FValidationEntry>> Selection;
	if (ListView.IsValid()) {
		Selection = ListView->GetSelectedItems();
	}

	if (Selection.Num() != 1 || !Selection[0].IsValid()) {
		return SNullWidget::NullWidget;
	}

	const TSharedPtr<FValidationEntry> Entry = Selection[0];

	TSharedRef<SVerticalBox> Locations = SNew(SVerticalBox);

	/* A valid asset never has suggestions, so this has to key off the status rather than
	 * the suggestion count, or being in the right place reads as being nowhere */
	if (Entry->Status == EValidationStatus::Valid) {
		Locations->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DetailsValid", "This asset is where it belongs."))
			.ColorAndOpacity(FSlateColor(GetValidationStatusColor(EValidationStatus::Valid)))
		];
	} else if (Entry->Suggestions.Num() == 0) {
		Locations->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("DetailsNoMatch", "Nothing in the game carries this asset name."))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	} else {
		for (const FString& Suggestion : Entry->Suggestions) {
			Locations->AddSlot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 1.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Suggestion))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
				[
					SNew(SButton)
					.Text(LOCTEXT("DetailsCopy", "Copy"))
					.OnClicked_Lambda([Suggestion] {
						CopyToClipboard(Suggestion);

						return FReply::Handled();
					})
				]
			];
		}
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(8.0f, 6.0f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Entry->PackagePath))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 2.0f, 0.0f, 6.0f))
			[
				SNew(STextBlock)
				.Text(GetResolvedPathText(Entry))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				Locations
			]
		];
}

/* Actions ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

void SValidationPanel::BrowseToEntry(TSharedPtr<FValidationEntry> Entry) {
	if (!Entry.IsValid()) {
		return;
	}

	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *(Entry->PackagePath + TEXT(".") + Entry->AssetName));
	if (Asset == nullptr) {
		UE_LOG(LogReflection, Warning, TEXT("Validation couldn't load '%s' to browse to it."), *Entry->PackagePath);

		return;
	}

	BrowseToAsset(Asset);
}

void SValidationPanel::CopyToClipboard(FString Text) {
	FPlatformApplicationMisc::ClipboardCopy(*Text);
}

void SValidationPanel::MoveEntryTo(TSharedPtr<FValidationEntry> Entry, FString Destination) {
	if (!Entry.IsValid() || Destination.IsEmpty()) {
		return;
	}

	bool bConfirmed = false;

	SpawnYesNoPrompt(
		TEXT("Move Asset"),
		FString::Printf(TEXT("Move\n\n%s\n\ninto\n\n%s?"), *Entry->PackagePath, *Destination),
		[&bConfirmed](const bool bResponse) {
			bConfirmed = bResponse;
		}
	);

	if (!bConfirmed) {
		return;
	}

	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *(Entry->PackagePath + TEXT(".") + Entry->AssetName));
	if (Asset == nullptr) {
		SpawnPrompt(TEXT("Move Asset"), FString::Printf(TEXT("Couldn't load '%s'."), *Entry->PackagePath));

		return;
	}

	const FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");

	TArray<FAssetRenameData> Renames;
	Renames.Add(FAssetRenameData(Asset, Destination, Entry->AssetName));

	if (!AssetToolsModule.Get().RenameAssets(Renames)) {
		SpawnPrompt(TEXT("Move Asset"), FString::Printf(TEXT("Couldn't move '%s'."), *Entry->PackagePath));

		return;
	}

	/* The validation result is stale now, the asset lives at the suggested location */
	Entry->PackagePath = Destination + TEXT("/") + Entry->AssetName;
	Entry->Status = EValidationStatus::Valid;
	Entry->Suggestions.Reset();
}

/* Text ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

FText SValidationPanel::GetSummaryText() const {
	if (!Validation.IsValid() || Validation->GetEntries().Num() == 0) {
		return LOCTEXT("SummaryIdle", "Run validation to check this project's content against the game.");
	}

	return FText::Format(
		LOCTEXT("SummaryFmt", "{0} shown  •  {1} misplaced  •  {2} missing  •  {3} valid"),
		FText::AsNumber(VisibleEntries.Num()),
		FText::AsNumber(Validation->GetCount(EValidationStatus::Misplaced)),
		FText::AsNumber(Validation->GetCount(EValidationStatus::Missing)),
		FText::AsNumber(Validation->GetCount(EValidationStatus::Valid))
	);
}

FText SValidationPanel::GetProgressText() const {
	if (!Validation.IsValid()) {
		return FText::GetEmpty();
	}

	return FText::Format(
		LOCTEXT("ProgressFmt", "{0} / {1}"),
		FText::AsNumber(Validation->GetCheckedCount()),
		FText::AsNumber(Validation->GetEntries().Num())
	);
}

TOptional<float> SValidationPanel::GetProgress() const {
	return Validation.IsValid() ? Validation->GetProgress() : 0.0f;
}

EVisibility SValidationPanel::GetProgressVisibility() const {
	return IsRunning() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SValidationPanel::GetFailureVisibility() const {
	return Validation.IsValid() && !Validation->GetFailureReason().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
}

FText SValidationPanel::GetFailureText() const {
	return Validation.IsValid() ? FText::FromString(Validation->GetFailureReason()) : FText::GetEmpty();
}

#undef LOCTEXT_NAMESPACE

#endif
