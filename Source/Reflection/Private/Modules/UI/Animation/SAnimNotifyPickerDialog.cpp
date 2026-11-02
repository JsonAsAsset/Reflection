/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/Animation/SAnimNotifyPickerDialog.h"

#include "Engine/Compatibility.h"

#include "Interfaces/IMainFrameModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "Reflection.AnimNotifyPicker"

bool SAnimNotifyPickerDialog::Open(const FString& Sequence, TArray<FAnimNotifyClassEntry>& InOutEntries) {
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("Title", "Reflect Notifies"))
		.ClientSize(FVector2D(560.0f, 420.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	TSharedPtr<SAnimNotifyPickerDialog> Dialog;

	Window->SetContent(
		SAssignNew(Dialog, SAnimNotifyPickerDialog)
		.ParentWindow(Window)
		.Sequence(Sequence)
		.Entries(InOutEntries)
	);

	const IMainFrameModule& MainFrameModule = IMainFrameModule::Get();
	FSlateApplication::Get().AddModalWindow(Window, MainFrameModule.GetParentWindow());

	if (!Dialog->Accepted) {
		return false;
	}

	InOutEntries.Reset(Dialog->Entries.Num());

	for (const FRowItem& Entry : Dialog->Entries) {
		if (Entry.IsValid()) {
			InOutEntries.Add(*Entry);
		}
	}

	return true;
}

void SAnimNotifyPickerDialog::Construct(const FArguments& InArgs) {
	ParentWindow = InArgs._ParentWindow;
	Sequence = InArgs._Sequence;

	for (const FAnimNotifyClassEntry& Entry : InArgs._Entries) {
		Entries.Add(MakeShared<FAnimNotifyClassEntry>(Entry));
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 8.0f, 8.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text(this, &SAnimNotifyPickerDialog::GetSummaryText)
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(FMargin(8.0f, 6.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(2.0f))
			[
				SAssignNew(ListView, SListView<FRowItem>)
				.ListItemsSource(&Entries)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SAnimNotifyPickerDialog::GenerateRow)
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 0.0f, 8.0f, 8.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text(LOCTEXT("All", "All"))
				.ContentPadding(FMargin(8.0f, 2.0f))
				.OnClicked(this, &SAnimNotifyPickerDialog::OnAllClicked)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("None", "None"))
				.ContentPadding(FMargin(8.0f, 2.0f))
				.OnClicked(this, &SAnimNotifyPickerDialog::OnNoneClicked)
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Center)
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(this, &SAnimNotifyPickerDialog::GetAcceptText)
				.ContentPadding(FMargin(8.0f, 2.0f))
				.IsEnabled(this, &SAnimNotifyPickerDialog::CanAccept)
				.OnClicked(this, &SAnimNotifyPickerDialog::OnAcceptClicked)
			]
		]
	];
}

TSharedRef<ITableRow> SAnimNotifyPickerDialog::GenerateRow(const FRowItem Item, const TSharedRef<STableViewBase>& OwnerTable) {
	if (!Item.IsValid()) {
		return SNew(STableRow<FRowItem>, OwnerTable);
	}

	return SNew(STableRow<FRowItem>, OwnerTable)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(6.0f, 4.0f, 0.0f, 4.0f))
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([Item] {
					return Item->Chosen ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([Item](const ECheckBoxState NewState) {
					Item->Chosen = NewState == ECheckBoxState::Checked;
				})
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(FMargin(6.0f, 4.0f))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->Class))
				]

				/* What the track calls them, which is how they are recognised in the editor and
				 * not always anything like the class name */
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->Named))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(6.0f, 4.0f, 8.0f, 4.0f))
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("CountFmt", "{0}"), FText::AsNumber(Item->Count)))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

/* Actions ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
FReply SAnimNotifyPickerDialog::OnAllClicked() {
	for (const FRowItem& Entry : Entries) {
		if (Entry.IsValid()) Entry->Chosen = true;
	}

	return FReply::Handled();
}

FReply SAnimNotifyPickerDialog::OnNoneClicked() {
	for (const FRowItem& Entry : Entries) {
		if (Entry.IsValid()) Entry->Chosen = false;
	}

	return FReply::Handled();
}

FReply SAnimNotifyPickerDialog::OnAcceptClicked() {
	Accepted = true;

	if (ParentWindow.IsValid()) {
		ParentWindow->RequestDestroyWindow();
	}

	return FReply::Handled();
}

/* State ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
bool SAnimNotifyPickerDialog::CanAccept() const {
	return CountChosen() > 0;
}

int32 SAnimNotifyPickerDialog::CountChosen() const {
	int32 Chosen = 0;

	for (const FRowItem& Entry : Entries) {
		if (Entry.IsValid() && Entry->Chosen) Chosen++;
	}

	return Chosen;
}

FText SAnimNotifyPickerDialog::GetSummaryText() const {
	int32 Notifies = 0;

	for (const FRowItem& Entry : Entries) {
		if (Entry.IsValid()) Notifies += Entry->Count;
	}

	return FText::Format(
		LOCTEXT("SummaryFmt", "The game's \"{0}\" carries {1} notify(s) of {2} class(es). A ticked class takes the place of whatever the animation in this project already has of that class; everything else on it is left alone."),
		FText::FromString(Sequence),
		FText::AsNumber(Notifies),
		FText::AsNumber(Entries.Num())
	);
}

FText SAnimNotifyPickerDialog::GetAcceptText() const {
	const int32 Chosen = CountChosen();

	if (Chosen == 0) {
		return LOCTEXT("Add", "Add");
	}

	return FText::Format(LOCTEXT("AddCountFmt", "Add {0}"), FText::AsNumber(Chosen));
}

#undef LOCTEXT_NAMESPACE
