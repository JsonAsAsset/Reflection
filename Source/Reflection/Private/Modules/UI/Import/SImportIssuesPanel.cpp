/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/Import/SImportIssuesPanel.h"

#include "Engine/Compatibility.h"
#include "Engine/EngineUtilities.h"
#include "Engine/Log.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Interfaces/IMainFrameModule.h"

#include "Styling/CoreStyle.h"
#include "Styling/SlateIconFinder.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Reflection.ImportIssues"

namespace {
	/* Reused across runs so a second import updates the window instead of stacking another one */
	TWeakPtr<SWindow> GWindow;
	TWeakPtr<SImportIssuesPanel> GPanel;

	const FSlateBrush* FindClassIcon(const FString& Type) {
		return FSlateIconFinder::FindCustomIconBrushForClass(FindObject<UClass>(nullptr, *("/Script/Engine." + Type)), TEXT("ClassThumbnail"));
	}
}

/* Row ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

class SImportIssueTableRow : public STableRow<TSharedPtr<FImportIssueRow>> {
public:
	SLATE_BEGIN_ARGS(SImportIssueTableRow) {}
		SLATE_ARGUMENT(TSharedPtr<FImportIssueRow>, Row)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable) {
		Row = InArgs._Row;

		STableRow::Construct(
			STableRow::FArguments()
			.Padding(FMargin(0.0f, 2.0f))
			.Content()
			[
				Row.IsValid() && Row->IsAsset() ? BuildAsset() : BuildIssue()
			],
			OwnerTable
		);
	}

private:
	TSharedRef<SWidget> BuildAsset() {
		const TSharedPtr<FImportIssueAsset> Asset = Row->Asset;

		if (!Asset.IsValid()) {
			return SNullWidget::NullWidget;
		}

		int32 Count = 0;

		for (const TSharedPtr<FImportIssue>& Issue : Asset->Issues) {
			Count += Issue->Count;
		}

		/* The tree row builds the expander itself, so this starts at the icon */
		return SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
			[
				SNew(SBox)
				.WidthOverride(16.0f)
				.HeightOverride(16.0f)
				[
					SNew(SImage)
					.Image(FindClassIcon(Asset->Type))
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Asset->Name))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				.ToolTipText(FText::FromString(Asset->Path.IsEmpty() ? Asset->Name : Asset->Path))
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(FMargin(8.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(Asset->Path))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(8.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("IssueCountFmt", "{0} error(s)"), FText::AsNumber(Count)))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
	}

	TSharedRef<SWidget> BuildIssue() const {
		const TSharedPtr<FImportIssue> Issue = Row.IsValid() ? Row->Issue : nullptr;

		if (!Issue.IsValid()) {
			return SNullWidget::NullWidget;
		}

		const FText Summary = Issue->Count > 1
			? FText::Format(LOCTEXT("IssueRepeatFmt", "{0} (x{1})"), FText::FromString(Issue->Summary), FText::AsNumber(Issue->Count))
			: FText::FromString(Issue->Summary);

		TSharedRef<SVerticalBox> Text = SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(Summary)
				.AutoWrapText(true)
			];

		if (!Issue->Detail.IsEmpty()) {
			Text->AddSlot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 2.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(Issue->Detail))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				];
		}

		return SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SBox)
				.MinDesiredWidth(90.0f)
				[
					SNew(STextBlock)
					.Text(GetImportIssueText(Issue->Kind))
					.ColorAndOpacity(FSlateColor(GetImportIssueColor(Issue->Kind)))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				]
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				Text
			];
	}

	TSharedPtr<FImportIssueRow> Row;
};

/* Panel ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

void SImportIssuesPanel::Construct(const FArguments& InArgs) {
	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(8.0f, 6.0f))
			[
				BuildHeader()
			]
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(FMargin(8.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
			[
				SAssignNew(TreeView, STreeView<TSharedPtr<FImportIssueRow>>)
				.TreeItemsSource(&Rows)
				.OnGenerateRow(this, &SImportIssuesPanel::GenerateRow)
				.OnGetChildren(this, &SImportIssuesPanel::GetRowChildren)
				.OnMouseButtonDoubleClick(this, &SImportIssuesPanel::OnRowDoubleClicked)
				.SelectionMode(ESelectionMode::Single)
			]
		]
	];

	Refresh();
}

TSharedRef<SWidget> SImportIssuesPanel::BuildHeader() {
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(this, &SImportIssuesPanel::GetSummaryText)
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
		[
			SNew(SButton)
			.Text(LOCTEXT("ExpandAll", "Expand All"))
			.OnClicked(this, &SImportIssuesPanel::OnExpandAllClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
		[
			SNew(SButton)
			.Text(LOCTEXT("CollapseAll", "Collapse All"))
			.OnClicked(this, &SImportIssuesPanel::OnCollapseAllClicked)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
		[
			SNew(SButton)
			.Text(LOCTEXT("CopyReport", "Copy Report"))
			.OnClicked(this, &SImportIssuesPanel::OnCopyReportClicked)
		];
}

void SImportIssuesPanel::Refresh() {
	Rows.Reset();

	for (const TSharedPtr<FImportIssueAsset>& Asset : FImportIssues::GetAssets()) {
		const TSharedPtr<FImportIssueRow> AssetRow = MakeShared<FImportIssueRow>();
		AssetRow->Asset = Asset;

		for (const TSharedPtr<FImportIssue>& Issue : Asset->Issues) {
			const TSharedPtr<FImportIssueRow> IssueRow = MakeShared<FImportIssueRow>();

			IssueRow->Asset = Asset;
			IssueRow->Issue = Issue;

			AssetRow->Children.Add(IssueRow);
		}

		Rows.Add(AssetRow);
	}

	if (!TreeView.IsValid()) {
		return;
	}

	TreeView->RequestTreeRefresh();

	/* One asset is its own answer, so it opens rather than making the user click it */
	if (Rows.Num() == 1) {
		TreeView->SetItemExpansion(Rows[0], true);
	}
}

TSharedRef<ITableRow> SImportIssuesPanel::GenerateRow(TSharedPtr<FImportIssueRow> Row, const TSharedRef<STableViewBase>& OwnerTable) {
	return SNew(SImportIssueTableRow, OwnerTable)
		.Row(Row);
}

void SImportIssuesPanel::GetRowChildren(TSharedPtr<FImportIssueRow> Row, TArray<TSharedPtr<FImportIssueRow>>& OutChildren) {
	if (Row.IsValid()) {
		OutChildren = Row->Children;
	}
}

void SImportIssuesPanel::OnRowDoubleClicked(TSharedPtr<FImportIssueRow> Row) {
	if (!Row.IsValid() || !Row->Asset.IsValid()) {
		return;
	}

	const TSharedPtr<FImportIssueAsset> Asset = Row->Asset;

	/* An asset that failed outright has nothing to browse to */
	if (Asset->Path.IsEmpty()) {
		return;
	}

	if (UObject* Object = LoadObjectByPath<UObject>(Asset->Path + TEXT(".") + Asset->Name)) {
		BrowseToAsset(Object);

		return;
	}

	/* Expanding is the next most useful thing a double click can do */
	if (TreeView.IsValid() && Row->IsAsset()) {
		TreeView->SetItemExpansion(Row, !TreeView->IsItemExpanded(Row));
	}
}

FReply SImportIssuesPanel::OnExpandAllClicked() {
	if (TreeView.IsValid()) {
		for (const TSharedPtr<FImportIssueRow>& Row : Rows) {
			TreeView->SetItemExpansion(Row, true);
		}
	}

	return FReply::Handled();
}

FReply SImportIssuesPanel::OnCollapseAllClicked() {
	if (TreeView.IsValid()) {
		for (const TSharedPtr<FImportIssueRow>& Row : Rows) {
			TreeView->SetItemExpansion(Row, false);
		}
	}

	return FReply::Handled();
}

FReply SImportIssuesPanel::OnCopyReportClicked() {
	const FString Report = BuildReport();

	FPlatformApplicationMisc::ClipboardCopy(*Report);

	return FReply::Handled();
}

FText SImportIssuesPanel::GetSummaryText() const {
	int32 Count = 0;

	for (const TSharedPtr<FImportIssueRow>& Row : Rows) {
		for (const TSharedPtr<FImportIssue>& Issue : Row->Asset->Issues) {
			Count += Issue->Count;
		}
	}

	return FText::Format(
		LOCTEXT("SummaryFmt", "{0} error(s) across {1} asset(s)"),
		FText::AsNumber(Count),
		FText::AsNumber(Rows.Num())
	);
}

/* Built from the rows rather than the collector, so it says what is on screen */
FString SImportIssuesPanel::BuildReport() const {
	FString Report;

	for (const TSharedPtr<FImportIssueRow>& Row : Rows) {
		const TSharedPtr<FImportIssueAsset> Asset = Row->Asset;

		Report += Asset->Name;

		if (!Asset->Type.IsEmpty()) {
			Report += TEXT(" (") + Asset->Type + TEXT(")");
		}

		if (!Asset->Path.IsEmpty()) {
			Report += TEXT(" - ") + Asset->Path;
		}

		Report += LINE_TERMINATOR;

		for (const TSharedPtr<FImportIssue>& Issue : Asset->Issues) {
			Report += TEXT("    [") + GetImportIssueText(Issue->Kind).ToString() + TEXT("] ") + Issue->Summary;

			if (Issue->Count > 1) {
				Report += FString::Printf(TEXT(" (x%d)"), Issue->Count);
			}

			if (!Issue->Detail.IsEmpty()) {
				Report += LINE_TERMINATOR + FString(TEXT("        ")) + Issue->Detail;
			}

			Report += LINE_TERMINATOR;
		}

		Report += LINE_TERMINATOR;
	}

	return Report;
}

void SImportIssuesPanel::Open() {
	if (!FSlateApplication::IsInitialized()) {
		return;
	}

	/* Already up: a second run refreshes it rather than opening another */
	if (const TSharedPtr<SWindow> Existing = GWindow.Pin()) {
		if (const TSharedPtr<SImportIssuesPanel> Panel = GPanel.Pin()) {
			Panel->Refresh();
		}

		Existing->BringToFront();

		return;
	}

	TSharedPtr<SImportIssuesPanel> Panel;

	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("WindowTitle", "Reflection Errors"))
		.ClientSize(FVector2D(820.0f, 520.0f))
		.SupportsMinimize(false);

	Window->SetContent(
		SAssignNew(Panel, SImportIssuesPanel)
	);

	GWindow = Window;
	GPanel = Panel;

	/* Not modal: the point of it is being able to go and look at what it names */
	IMainFrameModule* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>("MainFrame");
	const TSharedPtr<SWindow> Parent = MainFrame != nullptr ? MainFrame->GetParentWindow() : nullptr;

	if (Parent.IsValid()) {
		FSlateApplication::Get().AddWindowAsNativeChild(Window, Parent.ToSharedRef());

		return;
	}

	FSlateApplication::Get().AddWindow(Window);
}

#undef LOCTEXT_NAMESPACE
