/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/Reflect/SReflectTypeFilterDialog.h"

#include "Engine/Compatibility.h"

#include "Importers/Constructor/Types.h"

#include "Interfaces/IMainFrameModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "Reflection.ReflectTypeFilter"

bool SReflectTypeFilterDialog::Open(TSet<FString>& InOutAllowedTypes) {
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("Title", "Reflect Types"))
		.ClientSize(FVector2D(520.0f, 560.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	TSharedPtr<SReflectTypeFilterDialog> Dialog;

	Window->SetContent(
		SAssignNew(Dialog, SReflectTypeFilterDialog)
		.ParentWindow(Window)
		.AllowedTypes(InOutAllowedTypes)
	);

	const IMainFrameModule& MainFrameModule = IMainFrameModule::Get();
	FSlateApplication::Get().AddModalWindow(Window, MainFrameModule.GetParentWindow());

	if (!Dialog->Accepted) {
		return false;
	}

	TSet<FString> Allowed;

	for (const TSharedPtr<FReflectTypeFilterEntry>& Entry : Dialog->AllEntries) {
		if (Entry->Allowed) {
			Allowed.Add(Entry->Asset->Type);
		}
	}

	/* Measured against what could be ticked, not the whole list: the untickable rows would
	 * otherwise keep the count short forever and every run would carry a filter */
	InOutAllowedTypes = Allowed.Num() == Dialog->CountAvailable() ? TSet<FString>() : Allowed;

	return true;
}

void SReflectTypeFilterDialog::Construct(const FArguments& InArgs) {
	ParentWindow = InArgs._ParentWindow;

	const TSet<FString>& Allowed = InArgs._AllowedTypes;

	for (const TSharedPtr<FSupportedAssetEntry>& Asset : SSupportedAssetsPanel::Collect()) {
		TSharedPtr<FReflectTypeFilterEntry> Entry = MakeShared<FReflectTypeFilterEntry>();

		Entry->Asset = Asset;

		/* A folder run only ever asks Cloud, so a type Cloud will not send cannot come out of one
		 * however it is ticked. CanImport is not used for this: it turns textures down because
		 * they are built somewhere else entirely, and they do come out of a folder run. */
		if (!ImportTypes::Cloud::Allowed(Asset->Type)) {
			Entry->Available = false;
			Entry->Unavailable = TEXT("Cloud never sends this type, only a local json dump has it");
		} else if (!ImportTypes::Allowed(Asset->Type)) {
			Entry->Available = false;
			Entry->Unavailable = TEXT("Experimental, turn experiments on in the settings to reach it");
		}

		/* Nothing allowed means nothing was filtered, not that everything is off */
		Entry->Allowed = Entry->Available && (Allowed.Num() == 0 || Allowed.Contains(Asset->Type));

		AllEntries.Add(Entry);
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		/* Search ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 8.0f, 8.0f, 4.0f))
		[
			SNew(SSearchBox)
			.HintText(LOCTEXT("SearchHint", "Search types"))
			.OnTextChanged(this, &SReflectTypeFilterDialog::OnSearchChanged)
		]

		/* Types ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(FMargin(8.0f, 4.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(2.0f))
			[
				SAssignNew(ListView, SListView<TSharedPtr<FReflectTypeFilterEntry>>)
				.ListItemsSource(&VisibleEntries)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SReflectTypeFilterDialog::GenerateRow)
			]
		]

		/* Footer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 0.0f, 8.0f, 8.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SReflectTypeFilterDialog::GetSummaryText)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("All", "All"))
				.ToolTipText(LOCTEXT("AllTooltip", "Tick everything the search is showing."))
				.OnClicked(this, &SReflectTypeFilterDialog::OnAllClicked)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("None", "None"))
				.ToolTipText(LOCTEXT("NoneTooltip", "Untick everything the search is showing."))
				.OnClicked(this, &SReflectTypeFilterDialog::OnNoneClicked)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(12.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("Accept", "Done"))
				.OnClicked(this, &SReflectTypeFilterDialog::OnAcceptClicked)
			]
		]
	];

	ApplyFilter();
}

/* Rows ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
TSharedRef<ITableRow> SReflectTypeFilterDialog::GenerateRow(TSharedPtr<FReflectTypeFilterEntry> Entry, const TSharedRef<STableViewBase>& OwnerTable) {
	return SNew(STableRow<TSharedPtr<FReflectTypeFilterEntry>>, OwnerTable)
		.Padding(FMargin(4.0f, 2.0f))
		[
			SNew(SCheckBox)
			.IsEnabled(Entry->Available)
			.ToolTipText(Entry->Available ? FText::GetEmpty() : FText::FromString(Entry->Unavailable))
			.IsChecked_Lambda([Entry] {
				return Entry->Allowed ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([Entry](const ECheckBoxState NewState) {
				Entry->Allowed = NewState == ECheckBoxState::Checked;
			})
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(Entry->Asset->DisplayType))

					/* The stored type is what a Cloud export carries, so it belongs somewhere
					 * even when the row shows the friendlier name */
					.ToolTipText(FText::FromString(Entry->Asset->Type))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(8.0f, 0.0f, 4.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(Entry->Available ? Entry->Asset->Category : Entry->Unavailable))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		];
}

/* Search ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
void SReflectTypeFilterDialog::OnSearchChanged(const FText& Text) {
	SearchText = Text.ToString().TrimStartAndEnd();

	ApplyFilter();
}

void SReflectTypeFilterDialog::ApplyFilter() {
	VisibleEntries.Reset();

	for (const TSharedPtr<FReflectTypeFilterEntry>& Entry : AllEntries) {
		/* Matched against the full type too, so searching the name off a Cloud export still lands */
		if (SearchText.IsEmpty()
			|| Entry->Asset->Type.Contains(SearchText)
			|| Entry->Asset->DisplayType.Contains(SearchText)
			|| Entry->Asset->Category.Contains(SearchText)) {
			VisibleEntries.Add(Entry);
		}
	}

	if (ListView.IsValid()) {
		ListView->RequestListRefresh();
	}
}

/* Actions ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
FReply SReflectTypeFilterDialog::OnAllClicked() {
	for (const TSharedPtr<FReflectTypeFilterEntry>& Entry : VisibleEntries) {
		if (Entry->Available) {
			Entry->Allowed = true;
		}
	}

	return FReply::Handled();
}

FReply SReflectTypeFilterDialog::OnNoneClicked() {
	for (const TSharedPtr<FReflectTypeFilterEntry>& Entry : VisibleEntries) {
		Entry->Allowed = false;
	}

	return FReply::Handled();
}

FReply SReflectTypeFilterDialog::OnAcceptClicked() {
	Accepted = true;

	if (ParentWindow.IsValid()) {
		ParentWindow->RequestDestroyWindow();
	}

	return FReply::Handled();
}

/* State ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
int32 SReflectTypeFilterDialog::CountAllowed() const {
	int32 Allowed = 0;

	for (const TSharedPtr<FReflectTypeFilterEntry>& Entry : AllEntries) {
		if (Entry->Allowed) {
			Allowed++;
		}
	}

	return Allowed;
}

int32 SReflectTypeFilterDialog::CountAvailable() const {
	int32 Available = 0;

	for (const TSharedPtr<FReflectTypeFilterEntry>& Entry : AllEntries) {
		if (Entry->Available) {
			Available++;
		}
	}

	return Available;
}

FText SReflectTypeFilterDialog::GetSummaryText() const {
	const int32 Allowed = CountAllowed();
	const int32 Available = CountAvailable();

	if (Allowed == Available) {
		return LOCTEXT("SummaryAll", "Every type");
	}

	return FText::Format(
		LOCTEXT("SummaryFmt", "{0} of {1} types"),
		FText::AsNumber(Allowed),
		FText::AsNumber(Available)
	);
}

#undef LOCTEXT_NAMESPACE
