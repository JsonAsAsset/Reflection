/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/Reflect/SReflectFolderDialog.h"

#include "Engine/Compatibility.h"

#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Remote.h"
#include "Utilities/ContentBrowser.h"

#include "Interfaces/IMainFrameModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "Reflection.ReflectFolder"

bool SReflectFolderDialog::Open(TArray<FString>& OutPaths) {
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("Title", "Reflect Folder"))
		.ClientSize(FVector2D(720.0f, 480.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	TSharedPtr<SReflectFolderDialog> Dialog;

	Window->SetContent(
		SAssignNew(Dialog, SReflectFolderDialog)
		.ParentWindow(Window)
	);

	const IMainFrameModule& MainFrameModule = IMainFrameModule::Get();
	FSlateApplication::Get().AddModalWindow(Window, MainFrameModule.GetParentWindow());

	if (!Dialog->Accepted) {
		return false;
	}

	OutPaths = Dialog->Paths;

	return OutPaths.Num() > 0;
}

void SReflectFolderDialog::Construct(const FArguments& InArgs) {
	ParentWindow = InArgs._ParentWindow;

	ChildSlot
	[
		SNew(SVerticalBox)

		/* Folder ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(8.0f, 6.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SAssignNew(FolderBox, SEditableTextBox)
					.HintText(LOCTEXT("FolderHint", "/Game/Path/To/Folder"))
					.ToolTipText(LOCTEXT("FolderTooltip", "A folder of the game files, in either form: /Game/Foo or the way Cloud spells it."))
					.OnTextCommitted(this, &SReflectFolderDialog::OnFolderCommitted)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
				[
					SNew(SButton)
					.Text(LOCTEXT("UseSelected", "Use Selected"))
					.ToolTipText(this, &SReflectFolderDialog::GetSelectedFolderTooltip)
					.IsEnabled(this, &SReflectFolderDialog::HasSelectedFolder)
					.OnClicked(this, &SReflectFolderDialog::OnUseSelectedFolderClicked)
				]
			]
		]

		/* What was found ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 6.0f, 8.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text(this, &SReflectFolderDialog::GetStatusText)
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(FMargin(8.0f, 6.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(2.0f))
			[
				SAssignNew(ListView, SListView<TSharedPtr<FString>>)
				.ListItemsSource(&Rows)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SReflectFolderDialog::GenerateRow)
			]
		]

		/* Footer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 0.0f, 8.0f, 8.0f))
		[
			SNew(SHorizontalBox)

			/* Find sits away from the other two: it fills the list rather than acting on it */
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text(LOCTEXT("Find", "Find"))
				.ToolTipText(LOCTEXT("FindTooltip", "Ask Cloud what is under that folder"))
				.IsEnabled(this, &SReflectFolderDialog::CanFind)
				.OnClicked(this, &SReflectFolderDialog::OnFindClicked)
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNullWidget::NullWidget
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
			[
				SNew(SButton)
				.Text(this, &SReflectFolderDialog::GetReflectText)
				.IsEnabled(this, &SReflectFolderDialog::CanReflect)
				.OnClicked(this, &SReflectFolderDialog::OnReflectClicked)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Cancel", "Cancel"))
				.OnClicked(this, &SReflectFolderDialog::OnCancelClicked)
			]
		]
	];

	/* Whatever is selected in the Content Browser is usually what this was opened for */
	const FString SelectedFolder = GetSelectedContentBrowserFolder();

	if (!SelectedFolder.IsEmpty()) {
		FolderBox->SetText(FText::FromString(SelectedFolder));
	}

	FSlateApplication::Get().SetKeyboardFocus(FolderBox);
}

/* Actions ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
void SReflectFolderDialog::OnFolderCommitted(const FText& NewText, const ETextCommit::Type CommitType) {
	/* Enter in the box is the same as pressing Find, so a folder can be pasted and listed
	 * without reaching for the mouse */
	if (CommitType == ETextCommit::OnEnter) {
		Find();
	}
}

FReply SReflectFolderDialog::OnFindClicked() {
	Find();

	return FReply::Handled();
}

FReply SReflectFolderDialog::OnUseSelectedFolderClicked() {
	const FString SelectedFolder = GetSelectedContentBrowserFolder();

	if (!SelectedFolder.IsEmpty()) {
		FolderBox->SetText(FText::FromString(SelectedFolder));

		/* The folder changed under it, so what the list is showing is no longer the answer */
		Rows.Reset();
		Paths.Reset();
		Searched = false;

		ListView->RequestListRefresh();
	}

	return FReply::Handled();
}

FReply SReflectFolderDialog::OnReflectClicked() {
	Accepted = true;

	if (ParentWindow.IsValid()) {
		ParentWindow->RequestDestroyWindow();
	}

	return FReply::Handled();
}

FReply SReflectFolderDialog::OnCancelClicked() {
	if (ParentWindow.IsValid()) {
		ParentWindow->RequestDestroyWindow();
	}

	return FReply::Handled();
}

void SReflectFolderDialog::Find() {
	const FString Folder = FolderBox->GetText().ToString().TrimStartAndEnd();

	Rows.Reset();
	Paths.Reset();
	Searched = false;

	if (!Folder.IsEmpty()) {
		{
			const FBlockingRequestScope BlockingScope(FText::Format(
				LOCTEXT("ListingFolder", "Listing {0}"),
				FText::FromString(Folder)
			));

			Paths = Cloud::Folder::GetPathsBlocking(Folder);
		}

		for (const FString& Path : Paths) {
			Rows.Add(MakeShared<FString>(Path));
		}

		Searched = true;
	}

	ListView->RequestListRefresh();
}

/* State ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
bool SReflectFolderDialog::CanFind() const {
	return FolderBox.IsValid() && !FolderBox->GetText().ToString().TrimStartAndEnd().IsEmpty();
}

bool SReflectFolderDialog::CanReflect() const {
	return Paths.Num() > 0;
}

bool SReflectFolderDialog::HasSelectedFolder() const {
	return !GetSelectedContentBrowserFolder().IsEmpty();
}

FText SReflectFolderDialog::GetSelectedFolderTooltip() const {
	const FString SelectedFolder = GetSelectedContentBrowserFolder();

	if (SelectedFolder.IsEmpty()) {
		return LOCTEXT("UseSelectedNone", "Select a folder in the Content Browser to use it here.");
	}

	return FText::Format(LOCTEXT("UseSelectedFmt", "Use the folder selected in the Content Browser ({0})."), FText::FromString(SelectedFolder));
}

FText SReflectFolderDialog::GetStatusText() const {
	if (!Searched) {
		return LOCTEXT("StatusIdle", "Everything under the folder comes down, subfolders included.");
	}

	if (Paths.Num() == 0) {
		return LOCTEXT("StatusEmpty", "Cloud has nothing under that folder.");
	}

	return FText::Format(LOCTEXT("StatusFoundFmt", "{0} asset(s)"), FText::AsNumber(Paths.Num()));
}

FText SReflectFolderDialog::GetReflectText() const {
	if (Paths.Num() == 0) {
		return LOCTEXT("Reflect", "Reflect");
	}

	return FText::Format(LOCTEXT("ReflectCountFmt", "Reflect {0}"), FText::AsNumber(Paths.Num()));
}

TSharedRef<ITableRow> SReflectFolderDialog::GenerateRow(TSharedPtr<FString> Path, const TSharedRef<STableViewBase>& OwnerTable) const {
	return SNew(STableRow<TSharedPtr<FString>>, OwnerTable)
		[
			SNew(SBox)
			.Padding(FMargin(8.0f, 2.0f))
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Path.IsValid() ? *Path : FString()))
			]
		];
}

#undef LOCTEXT_NAMESPACE
