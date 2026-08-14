/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/UI/SupportedAssets/SSupportedAssetsPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SSearchBox;
class SWindow;

/* One supported type, plus whether the run is allowed to build it */
struct FReflectTypeFilterEntry {
	TSharedPtr<FSupportedAssetEntry> Asset;

	bool Allowed = true;

	/* Listed but untickable: a folder run is a Cloud run, and not every supported type is one
	 * Cloud will send. Shown rather than dropped, so a missing type never looks like an oversight. */
	bool Available = true;

	/* Why it cannot be ticked, straight onto the row */
	FString Unavailable;
};

/* Picks which asset types a folder run is allowed to build.
 *
 * The list is whatever the registries claim, so it says the same thing as the Supported Assets
 * window. Everything starts ticked: a filter nobody opened cannot change what a run does. */
class REFLECTION_API SReflectTypeFilterDialog : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(SReflectTypeFilterDialog) {}
		SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)

		/* Types allowed on the way in. Empty means no filter, so everything starts ticked. */
		SLATE_ARGUMENT(TSet<FString>, AllowedTypes)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* Runs modally. True when the user accepted, with InOutAllowedTypes holding what stayed
	 * ticked, or emptied when that turned out to be all of them. */
	static bool Open(TSet<FString>& InOutAllowedTypes);

private:
	TSharedRef<ITableRow> GenerateRow(TSharedPtr<FReflectTypeFilterEntry> Entry, const TSharedRef<STableViewBase>& OwnerTable);

	void OnSearchChanged(const FText& Text);
	void ApplyFilter();

	/* Ticks or unticks everything the search is currently showing, rather than the whole list:
	 * with a search typed, All is the only quick way to take one category */
	FReply OnAllClicked();
	FReply OnNoneClicked();

	FReply OnAcceptClicked();

	FText GetSummaryText() const;

	int32 CountAllowed() const;
	int32 CountAvailable() const;

	TArray<TSharedPtr<FReflectTypeFilterEntry>> AllEntries;
	TArray<TSharedPtr<FReflectTypeFilterEntry>> VisibleEntries;

	TSharedPtr<SListView<TSharedPtr<FReflectTypeFilterEntry>>> ListView;
	TSharedPtr<SWindow> ParentWindow;

	FString SearchText;

	bool Accepted = false;
};
