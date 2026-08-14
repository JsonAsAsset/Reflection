/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/Reflect/SReflectFolderDialog.h"

#include "Engine/Compatibility.h"

#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Remote.h"
#include "Utilities/ContentBrowser.h"
#include "Containers/Export.h"
#include "Utilities/Dialog.h"
#include "Modules/UI/Reflect/SReflectTypeFilterDialog.h"

#include "Interfaces/IMainFrameModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "Reflection.ReflectFolder"

/* Reduces whatever was pasted to the folder it lives in. */
static FString ToFolderPath(FString Path) {
	Path.TrimStartAndEndInline();
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));

	if (FString Leaf; Path.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd) && Leaf.Contains(TEXT("."))) {
		Path.Split(TEXT("/"), &Path, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	}

	Path.RemoveFromEnd(TEXT("/"));

	return Path;
}

bool SReflectFolderDialog::Open(TArray<FString>& OutPaths, TSet<FString>& OutAllowedTypes, const FString& InitialFolder) {
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("Title", "Reflect Folder"))
		.ClientSize(FVector2D(720.0f, 480.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	TSharedPtr<SReflectFolderDialog> Dialog;

	Window->SetContent(
		SAssignNew(Dialog, SReflectFolderDialog)
		.ParentWindow(Window)
		.InitialFolder(InitialFolder)
	);

	const IMainFrameModule& MainFrameModule = IMainFrameModule::Get();
	FSlateApplication::Get().AddModalWindow(Window, MainFrameModule.GetParentWindow());

	if (!Dialog->Accepted) {
		return false;
	}

	OutPaths = Dialog->Paths;
	OutAllowedTypes = Dialog->AllowedTypes;

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
			.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(8.0f, 6.0f))
			[
				SNew(SHorizontalBox)

				/* Fills the box, so it sits on the side the folder comes in from */
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
				[
					SNew(SButton)
					.Text(LOCTEXT("UseSelected", "Use Selected"))
					.ToolTipText(this, &SReflectFolderDialog::GetSelectedFolderTooltip)
					.IsEnabled(this, &SReflectFolderDialog::HasSelectedFolder)
					.OnClicked(this, &SReflectFolderDialog::OnUseSelectedFolderClicked)
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SAssignNew(FolderBox, SEditableTextBox)
					.HintText(LOCTEXT("FolderHint", "/Game/Folder"))
					.ToolTipText(LOCTEXT("FolderTooltip", "A folder of the game files, in either form: /Game/Foo or the way Cloud spells it."))
					.OnTextCommitted(this, &SReflectFolderDialog::OnFolderCommitted)
				]

				/* Next to the box it acts on */
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
				[
					SNew(SButton)
					.Text(LOCTEXT("Find", "Find"))
					.ToolTipText(LOCTEXT("FindTooltip", "Ask Cloud what is under that folder"))
					.IsEnabled(this, &SReflectFolderDialog::CanFind)
					.OnClicked(this, &SReflectFolderDialog::OnFindClicked)
				]
			]
		]

		/* What was found ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(FMargin(8.0f, 6.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
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

			/* Under the list it counts, on the same line as the button it qualifies */
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SReflectFolderDialog::GetStatusText)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(this, &SReflectFolderDialog::GetTypesText)
				.ToolTipText(LOCTEXT("TypesTooltip", "Choose which asset types the run is allowed to build. Every type by default.\n\nThis covers what the folder listed. Assets those reference still come down with them, or what did come down would be missing pieces."))
				.OnClicked(this, &SReflectFolderDialog::OnTypesClicked)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(this, &SReflectFolderDialog::GetReflectText)
				.IsEnabled(this, &SReflectFolderDialog::CanReflect)
				.OnClicked(this, &SReflectFolderDialog::OnReflectClicked)
			]
		]
	];

	FString SelectedFolder = InArgs._InitialFolder;

	if (SelectedFolder.IsEmpty()) {
		const FString Clipboard = GetClipboard().TrimStartAndEnd();

		if (IsAssetPathLike(Clipboard)) {
			/* Reduced before it goes in the box rather than inside Find, so what is shown is
			 * the folder that gets listed */
			SelectedFolder = ToFolderPath(StripObjectOuter(Clipboard));
		}
	}

	if (SelectedFolder.IsEmpty()) {
		SelectedFolder = GetSelectedContentBrowserFolder();
	}

	if (!SelectedFolder.IsEmpty()) {
		FolderBox->SetText(FText::FromString(SelectedFolder));

		/* The folder is already known, and the list is the point */
		Find();
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

FReply SReflectFolderDialog::OnTypesClicked() {
	SReflectTypeFilterDialog::Open(AllowedTypes);

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

void SReflectFolderDialog::Find() {
	const FString Folder = ToFolderPath(FolderBox->GetText().ToString());

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

FText SReflectFolderDialog::GetTypesText() const {
	/* The count is the only sign a filter is on once the window is shut */
	if (AllowedTypes.Num() == 0) {
		return LOCTEXT("TypesAll", "All Types");
	}

	return FText::Format(LOCTEXT("TypesSomeFmt", "{0} Types"), FText::AsNumber(AllowedTypes.Num()));
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
