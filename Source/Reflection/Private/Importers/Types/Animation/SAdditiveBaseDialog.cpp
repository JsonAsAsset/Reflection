/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Animation/SAdditiveBaseDialog.h"

#include "Engine/AssetCompatibility.h"
#include "Engine/Compatibility.h"
#include "Settings/Redirector.h"
#include "Utilities/Dialog.h"

#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetData.h"
#include "PropertyCustomizationHelpers.h"

#include "Framework/Application/SlateApplication.h"
#include "Interfaces/IMainFrameModule.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Reflection.AdditiveBase"

void SAdditiveBaseDialog::Construct(const FArguments& InArgs) {
	ParentWindow = InArgs._ParentWindow;
	SequenceName = InArgs._SequenceName;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(16.0f)
		[
			SNew(SVerticalBox)

			/* What is being asked, and why it cannot be answered from the export */
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::Format(
					LOCTEXT("Explanation",
						"\"{0}\" is additive against a frame of itself, so the animation it was built over wasn't kept.\n\n"
						"Name that animation and the difference is rebuilt over it. Without one it is rebuilt over the skeleton's own pose, which plays correctly but reads oddly on its own."),
					FText::FromString(SequenceName)))
				.AutoWrapText(true)
			]

			/* An animation already in the project */
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 16.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PickLabel", "An animation in this project"))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(UAnimSequence::StaticClass())
				.ObjectPath(this, &SAdditiveBaseDialog::GetPickedAssetPath)
				.OnObjectChanged(this, &SAdditiveBaseDialog::OnAssetPicked)
				.AllowClear(true)
				.DisplayThumbnail(false)
			]

			/* Or a path the Cloud can reach, for one that hasn't been reflected yet */
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("PathLabel", "Or a path the Cloud can reach"))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(PathBox, SEditableTextBox)
				.HintText(LOCTEXT("PathHint", "FortniteGame/Content/Animation/..."))
				.OnTextCommitted(this, &SAdditiveBaseDialog::OnPathCommitted)
			]

			/* What either of them came out as, since a picked asset is asked for by the path the
			 * game cooked it under rather than by where it landed here */
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SAdditiveBaseDialog::GetPickedPath)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNullWidget::NullWidget
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			.Padding(0.0f, 16.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Without", "Without one"))
					.OnClicked(this, &SAdditiveBaseDialog::OnWithoutClicked)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Use", "Build over this"))
					.IsEnabled(this, &SAdditiveBaseDialog::CanUse)
					.OnClicked(this, &SAdditiveBaseDialog::OnUseClicked)
				]
			]
		]
	];
}

FString SAdditiveBaseDialog::GetPickedAssetPath() const {
	return PickedAsset;
}

void SAdditiveBaseDialog::OnAssetPicked(const FAssetData& AssetData) {
	PickedAsset = AssetData.IsValid() ? GetAssetObjectPath(AssetData) : FString();

	/* One or the other: a path typed in is what the picker is for, said differently */
	if (!PickedAsset.IsEmpty() && PathBox.IsValid()) {
		PathBox->SetText(FText::GetEmpty());
		TypedPath.Empty();
	}
}

void SAdditiveBaseDialog::OnPathCommitted(const FText& NewText, ETextCommit::Type CommitType) {
	TypedPath = NewText.ToString().TrimStartAndEnd();

	if (!TypedPath.IsEmpty()) {
		PickedAsset.Empty();
	}
}

/* The path the Cloud is asked for.
 *
 * A path typed in is already the game's. An asset picked out of the project is not: it sits
 * wherever the import put it, so it is turned back into the path the game cooked it under, which
 * is the same turn the import itself makes to ask for its own geometry. */
FText SAdditiveBaseDialog::GetPickedPath() const {
	if (!TypedPath.IsEmpty()) {
		return FText::Format(LOCTEXT("AskingFor", "Asking the Cloud for: {0}"), FText::FromString(TypedPath));
	}

	if (PickedAsset.IsEmpty()) {
		return LOCTEXT("NothingPicked", "Nothing named yet");
	}

	FString Path = FPackageName::ObjectPathToPackageName(PickedAsset);
	FRRedirects::Reverse(Path);

	return FText::Format(LOCTEXT("AskingFor", "Asking the Cloud for: {0}"), FText::FromString(Path));
}

bool SAdditiveBaseDialog::CanUse() const {
	return !TypedPath.IsEmpty() || !PickedAsset.IsEmpty();
}

FReply SAdditiveBaseDialog::OnUseClicked() {
	Accepted = true;

	if (ParentWindow.IsValid()) {
		ParentWindow->RequestDestroyWindow();
	}

	return FReply::Handled();
}

FReply SAdditiveBaseDialog::OnWithoutClicked() {
	Accepted = false;

	if (ParentWindow.IsValid()) {
		ParentWindow->RequestDestroyWindow();
	}

	return FReply::Handled();
}

bool SAdditiveBaseDialog::Open(const FString& SequenceName, FString& OutBasePath) {
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("Title", "Reflection"))
		.ClientSize(FVector2D(560.0f, 340.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	TSharedPtr<SAdditiveBaseDialog> Dialog;

	Window->SetContent(
		SAssignNew(Dialog, SAdditiveBaseDialog)
		.ParentWindow(Window)
		.SequenceName(SequenceName)
	);

	const IMainFrameModule& MainFrameModule = IMainFrameModule::Get();
	FSlateApplication::Get().AddModalWindow(Window, MainFrameModule.GetParentWindow());

	if (!Dialog->Accepted) {
		return false;
	}

	OutBasePath = Dialog->TypedPath;

	if (OutBasePath.IsEmpty() && !Dialog->PickedAsset.IsEmpty()) {
		OutBasePath = FPackageName::ObjectPathToPackageName(Dialog->PickedAsset);

		FRRedirects::Reverse(OutBasePath);
	}

	return !OutBasePath.IsEmpty();
}

#undef LOCTEXT_NAMESPACE
