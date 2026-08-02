/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SSearchBox;

/* One asset type, as the list shows it */
struct FSupportedAssetEntry {
	/* The type as the registries name it, which is what a Cloud export carries */
	FString Type;

	/* The same type without the GeneratedClass suffix. Nothing ships an icon for
	 * BlueprintGeneratedClass, and the class it was generated from reads better anyway. */
	FString DisplayType;

	FString Category;

	/* Which importer picks it up */
	FString Importer;

	/* Hidden unless experiments are turned on in the settings */
	bool Experimental = false;

	/* Reachable from a local json dump, but never fetched from Cloud */
	bool CloudBlocked = false;
};

namespace SupportedAssetColumns {
	const FName Type(TEXT("Type"));
	const FName Category(TEXT("Category"));
	const FName Importer(TEXT("Importer"));
	const FName Notes(TEXT("Notes"));
}

/* Lists every asset type Reflection can build, read out of the registries themselves rather than
 * written down anywhere: the importer factory registry, the templated type table and the texture
 * classes. A type missing from this window is a type nothing has claimed. */
class REFLECTION_API SSupportedAssetsPanel : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(SSupportedAssetsPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* Reads the registries again, ex: after experiments get toggled */
	void Refresh();

private:
	TSharedRef<ITableRow> GenerateRow(TSharedPtr<FSupportedAssetEntry> Entry, const TSharedRef<STableViewBase>& OwnerTable);

	void OnSearchChanged(const FText& Text);
	void ApplyFilter();

	FText GetSummaryText() const;

	TArray<TSharedPtr<FSupportedAssetEntry>> AllEntries;
	TArray<TSharedPtr<FSupportedAssetEntry>> VisibleEntries;

	TSharedPtr<SListView<TSharedPtr<FSupportedAssetEntry>>> ListView;
	FString SearchText;
};
