/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SEditableTextBox;
class SWindow;
class UAnimSequence;

/* Asks which animation an additive was built over.
 *
 * A sequence that is a difference from a frame of itself has lost what it was a difference from:
 * the cook keeps the difference and nothing else, and the sequence points at itself rather than at
 * whatever it was authored over. Nothing in the export can say what that was, so it is asked for.
 *
 * Either an animation already in the project or a path the Cloud can reach: both come back as the
 * path the game cooked the animation under, which is what the Cloud is asked for. Answering with
 * nothing is a fair answer the difference is then built over the skeleton's own pose, which
 * plays correctly and reads oddly on its own. */
class REFLECTION_API SAdditiveBaseDialog : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(SAdditiveBaseDialog) {}
		SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
		SLATE_ARGUMENT(FString, SequenceName)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* Runs the dialog modally. True when the user named an animation, with OutBasePath holding the
	 * path to ask the Cloud for; false when they chose to go without one. */
	static bool Open(const FString& SequenceName, FString& OutBasePath);

private:
	FReply OnUseClicked();
	FReply OnWithoutClicked();

	bool CanUse() const;

	FText GetPickedPath() const;
	FString GetPickedAssetPath() const;

	void OnAssetPicked(const FAssetData& AssetData);
	void OnPathCommitted(const FText& NewText, ETextCommit::Type CommitType);

	TSharedPtr<SWindow> ParentWindow;
	TSharedPtr<SEditableTextBox> PathBox;

	FString SequenceName;

	/* What the picker holds, as a project path */
	FString PickedAsset;

	/* What the box holds, which is a game path already */
	FString TypedPath;

	bool Accepted = false;
};
