/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/Reflect/SReflectPathsDialog.h"

#include "Engine/Compatibility.h"

#include "Utilities/Dialog.h"

#include "Interfaces/IMainFrameModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "Reflection.ReflectPaths"

bool SReflectPathsDialog::Open(TArray<FString>& OutPaths) {
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("Title", "Reflect From Path"))
		.ClientSize(FVector2D(720.0f, 480.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	TSharedPtr<SReflectPathsDialog> Dialog;

	Window->SetContent(
		SAssignNew(Dialog, SReflectPathsDialog)
		.ParentWindow(Window)
	);

	const IMainFrameModule& MainFrameModule = IMainFrameModule::Get();
	FSlateApplication::Get().AddModalWindow(Window, MainFrameModule.GetParentWindow());

	if (!Dialog->Accepted) {
		return false;
	}

	OutPaths.Reset();

	for (const TSharedPtr<FString>& Row : Dialog->Rows) {
		if (Row.IsValid()) {
			OutPaths.Add(*Row);
		}
	}

	return OutPaths.Num() > 0;
}

void SReflectPathsDialog::Construct(const FArguments& InArgs) {
	ParentWindow = InArgs._ParentWindow;

	ChildSlot
	[
		SNew(SVerticalBox)

		/* Entry ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
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
					SAssignNew(PathBox, SEditableTextBox)
					.HintText(LOCTEXT("PathHint", "/Game/Path/To/Asset"))
					.ToolTipText(LOCTEXT("PathTooltip", "An asset path, in either form. A copied reference or several paths at once are fine."))
					.OnTextCommitted(this, &SReflectPathsDialog::OnPathCommitted)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
				[
					SNew(SButton)
					.Text(LOCTEXT("Add", "Add"))
					.ToolTipText(LOCTEXT("AddTooltip", "Queue this path. Several pasted at once are queued one per line."))
					.IsEnabled(this, &SReflectPathsDialog::CanAdd)
					.OnClicked(this, &SReflectPathsDialog::OnAddClicked)
				]
			]
		]

		/* Queue ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 6.0f, 8.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text(this, &SReflectPathsDialog::GetStatusText)
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
				.SelectionMode(ESelectionMode::Multi)
				.OnGenerateRow(this, &SReflectPathsDialog::GenerateRow)
			]
		]

		/* Footer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Right)
		.Padding(FMargin(8.0f, 0.0f, 8.0f, 8.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("Remove", "Remove"))
				.ToolTipText(LOCTEXT("RemoveTooltip", "Take the selected paths back out of the queue"))
				.IsEnabled(this, &SReflectPathsDialog::CanRemove)
				.OnClicked(this, &SReflectPathsDialog::OnRemoveClicked)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
			[
				SNew(SButton)
				.Text(this, &SReflectPathsDialog::GetReflectText)
				.IsEnabled(this, &SReflectPathsDialog::CanReflect)
				.OnClicked(this, &SReflectPathsDialog::OnReflectClicked)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Cancel", "Cancel"))
				.OnClicked(this, &SReflectPathsDialog::OnCancelClicked)
			]
		]
	];

	/* The path is usually already on the clipboard, straight out of the asset it was copied from */
	const FString Clipboard = GetClipboard();

	if (Clipboard.Contains(TEXT("/"))) {
		AddPaths(Clipboard);
	}

	FSlateApplication::Get().SetKeyboardFocus(PathBox);
}

/* Actions ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
void SReflectPathsDialog::OnPathCommitted(const FText& NewText, const ETextCommit::Type CommitType) {
	/* Enter queues what was typed, so paths can be pasted one after another without the mouse */
	if (CommitType == ETextCommit::OnEnter) {
		AddFromBox();
	}
}

FReply SReflectPathsDialog::OnAddClicked() {
	AddFromBox();

	return FReply::Handled();
}

FReply SReflectPathsDialog::OnRemoveClicked() {
	for (const TSharedPtr<FString>& Selected : ListView->GetSelectedItems()) {
		Rows.Remove(Selected);
	}

	ListView->ClearSelection();
	ListView->RequestListRefresh();

	return FReply::Handled();
}

FReply SReflectPathsDialog::OnReflectClicked() {
	Accepted = true;

	if (ParentWindow.IsValid()) {
		ParentWindow->RequestDestroyWindow();
	}

	return FReply::Handled();
}

FReply SReflectPathsDialog::OnCancelClicked() {
	if (ParentWindow.IsValid()) {
		ParentWindow->RequestDestroyWindow();
	}

	return FReply::Handled();
}

void SReflectPathsDialog::AddFromBox() {
	AddPaths(PathBox->GetText().ToString());

	PathBox->SetText(FText::GetEmpty());

	FSlateApplication::Get().SetKeyboardFocus(PathBox);
}

void SReflectPathsDialog::AddPaths(const FString& Text) {
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines) {
		const FString Path = Line.TrimStartAndEnd();
		if (Path.IsEmpty()) continue;

		/* Reflecting the same path twice is just a wasted request */
		const bool Queued = Rows.ContainsByPredicate([&Path](const TSharedPtr<FString>& Row) {
			return Row.IsValid() && Row->Equals(Path, ESearchCase::IgnoreCase);
		});

		if (Queued) continue;

		Rows.Add(MakeShared<FString>(Path));
	}

	if (ListView.IsValid()) {
		ListView->RequestListRefresh();
	}
}

/* State ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
bool SReflectPathsDialog::CanAdd() const {
	return PathBox.IsValid() && !PathBox->GetText().ToString().TrimStartAndEnd().IsEmpty();
}

bool SReflectPathsDialog::CanRemove() const {
	return ListView.IsValid() && ListView->GetNumItemsSelected() > 0;
}

bool SReflectPathsDialog::CanReflect() const {
	return Rows.Num() > 0;
}

FText SReflectPathsDialog::GetStatusText() const {
	if (Rows.Num() == 0) {
		return LOCTEXT("StatusEmpty", "Nothing queued. Paths can be added one at a time, or pasted several at once.");
	}

	return FText::Format(LOCTEXT("StatusQueuedFmt", "{0} queued"), FText::AsNumber(Rows.Num()));
}

FText SReflectPathsDialog::GetReflectText() const {
	if (Rows.Num() == 0) {
		return LOCTEXT("Reflect", "Reflect");
	}

	return FText::Format(LOCTEXT("ReflectCountFmt", "Reflect {0}"), FText::AsNumber(Rows.Num()));
}

TSharedRef<ITableRow> SReflectPathsDialog::GenerateRow(TSharedPtr<FString> Path, const TSharedRef<STableViewBase>& OwnerTable) const {
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
