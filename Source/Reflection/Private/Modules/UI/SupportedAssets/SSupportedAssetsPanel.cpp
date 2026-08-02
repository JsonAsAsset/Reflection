/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/SupportedAssets/SSupportedAssetsPanel.h"

#include "Importers/Constructor/Types.h"
#include "Importers/Constructor/Registry/RegistrationInfo.h"
#include "Importers/Types/Texture/TextureTypes.h"

#include "Engine/Compatibility.h"
#include "Engine/EngineUtilities.h"

#include "Styling/SlateIconFinder.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "Reflection.SupportedAssets"

/* BlueprintGeneratedClass and the like are what a cooked package calls them, but the icon and the
 * name both belong to the class they were generated from */
static FString ToDisplayType(const FString& Type) {
	FString Display;

	if (Type.Split(TEXT("GeneratedClass"), &Display, nullptr) && !Display.IsEmpty()) {
		return Display;
	}

	return Type;
}

/* One row of the list */
class SSupportedAssetRow : public SMultiColumnTableRow<TSharedPtr<FSupportedAssetEntry>> {
public:
	SLATE_BEGIN_ARGS(SSupportedAssetRow) {}
		SLATE_ARGUMENT(TSharedPtr<FSupportedAssetEntry>, Entry)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable) {
		Entry = InArgs._Entry;

		SMultiColumnTableRow::Construct(FSuperRowType::FArguments(), OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override {
		if (!Entry.IsValid()) {
			return SNullWidget::NullWidget;
		}

		if (ColumnName == SupportedAssetColumns::Type) {
			/* Looked up by name across every module, so the ones living outside Engine still
			 * find their icon. Falls back to the default class icon when nothing matches. */
			const UClass* Class = FindClassByType(Entry->DisplayType);

			return SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SImage)
					.Image(FSlateIconFinder::FindIconForClass(Class).GetIcon())
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Entry->DisplayType))
					.ToolTipText(FText::FromString(Entry->Type))
				];
		}

		if (ColumnName == SupportedAssetColumns::Category) {
			return SNew(STextBlock)
				.Margin(FMargin(4.0f, 2.0f))
				.Text(FText::FromString(Entry->Category));
		}

		if (ColumnName == SupportedAssetColumns::Importer) {
			return SNew(STextBlock)
				.Margin(FMargin(4.0f, 2.0f))
				.Text(FText::FromString(Entry->Importer));
		}

		if (ColumnName == SupportedAssetColumns::Notes) {
			TArray<FString> Notes;

			if (Entry->Experimental) Notes.Add(TEXT("Experimental"));
			if (Entry->CloudBlocked) Notes.Add(TEXT("Local json only"));

			return SNew(STextBlock)
				.Margin(FMargin(4.0f, 2.0f))
				.Text(FText::FromString(FString::Join(Notes, TEXT(", "))));
		}

		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FSupportedAssetEntry> Entry;
};

void SSupportedAssetsPanel::Construct(const FArguments& InArgs) {
	Refresh();

	ChildSlot
	[
		SNew(SBorder)
		.Padding(8.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SSearchBox)
				.HintText(LOCTEXT("SearchHint", "Search asset types"))
				.OnTextChanged(this, &SSupportedAssetsPanel::OnSearchChanged)
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(ListView, SListView<TSharedPtr<FSupportedAssetEntry>>)
				.ListItemsSource(&VisibleEntries)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SSupportedAssetsPanel::GenerateRow)
				.HeaderRow(
					SNew(SHeaderRow)

					+ SHeaderRow::Column(SupportedAssetColumns::Type)
					.DefaultLabel(LOCTEXT("TypeColumn", "Asset Type"))
					.FillWidth(0.35f)

					+ SHeaderRow::Column(SupportedAssetColumns::Category)
					.DefaultLabel(LOCTEXT("CategoryColumn", "Category"))
					.FillWidth(0.25f)

					+ SHeaderRow::Column(SupportedAssetColumns::Importer)
					.DefaultLabel(LOCTEXT("ImporterColumn", "Importer"))
					.FillWidth(0.2f)

					+ SHeaderRow::Column(SupportedAssetColumns::Notes)
					.DefaultLabel(LOCTEXT("NotesColumn", "Notes"))
					.FillWidth(0.2f)
				)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(4.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SSupportedAssetsPanel::GetSummaryText)
			]
		]
	];
}

void SSupportedAssetsPanel::Refresh() {
	AllEntries.Reset();

	/* Keyed on type, because a type can be reached more than one way and the first claim wins */
	TMap<FString, TSharedPtr<FSupportedAssetEntry>> Seen;

	const auto Add = [&Seen, this](const FString& Type, const FString& Category, const FString& Importer) {
		if (Type.IsEmpty() || Seen.Contains(Type)) return;

		TSharedPtr<FSupportedAssetEntry> Entry = MakeShared<FSupportedAssetEntry>();

		Entry->Type = Type;
		Entry->DisplayType = ToDisplayType(Type);
		Entry->Category = Category;
		Entry->Importer = Importer;
		Entry->Experimental = ImportTypes::Experimental.Contains(Type);
		Entry->CloudBlocked = ImportTypes::Cloud::Blacklisted.Contains(Type);

		Seen.Add(Type, Entry);
		AllEntries.Add(Entry);
	};

	/* Types with an importer of their own */
	for (const TPair<TArray<FString>, FImporterRegistrationInfo>& Registration : GetFactoryRegistry()) {
		for (const FString& Type : Registration.Key) {
			Add(Type, Registration.Value.Category, TEXT("Dedicated"));
		}
	}

	/* Types the templated importer covers by deserializing their properties straight across */
	for (const TPair<FString, TArray<FString>>& Pair : ImportTypes::Templated) {
		for (const FString& Type : Pair.Value) {
			Add(Type, Pair.Key, TEXT("Templated"));
		}
	}

	/* Textures are rebuilt from pixels rather than properties, so they sit outside both */
	for (const FString& Type : FTextureTypes::All()) {
		Add(Type, TEXT("Texture Assets"), TEXT("Texture"));
	}

	AllEntries.Sort([](const TSharedPtr<FSupportedAssetEntry>& A, const TSharedPtr<FSupportedAssetEntry>& B) {
		if (A->Category != B->Category) {
			return A->Category < B->Category;
		}

		/* Sorted the way it reads, not the way it's stored */
		return A->DisplayType < B->DisplayType;
	});

	ApplyFilter();
}

void SSupportedAssetsPanel::OnSearchChanged(const FText& Text) {
	SearchText = Text.ToString().TrimStartAndEnd();

	ApplyFilter();
}

void SSupportedAssetsPanel::ApplyFilter() {
	VisibleEntries.Reset();

	for (const TSharedPtr<FSupportedAssetEntry>& Entry : AllEntries) {
		/* Matched against the full type too, so searching the name off a Cloud export still lands */
		if (SearchText.IsEmpty()
			|| Entry->Type.Contains(SearchText)
			|| Entry->DisplayType.Contains(SearchText)
			|| Entry->Category.Contains(SearchText)) {
			VisibleEntries.Add(Entry);
		}
	}

	if (ListView.IsValid()) {
		ListView->RequestListRefresh();
	}
}

FText SSupportedAssetsPanel::GetSummaryText() const {
	/* Data assets never get listed one by one, the importer takes the whole hierarchy */
	const FString Counts = VisibleEntries.Num() == AllEntries.Num()
		? FString::Printf(TEXT("%d asset types"), AllEntries.Num())
		: FString::Printf(TEXT("%d of %d asset types"), VisibleEntries.Num(), AllEntries.Num());

	return FText::FromString(Counts + TEXT(", plus anything deriving from Data Asset"));
}

TSharedRef<ITableRow> SSupportedAssetsPanel::GenerateRow(TSharedPtr<FSupportedAssetEntry> Entry, const TSharedRef<STableViewBase>& OwnerTable) {
	return SNew(SSupportedAssetRow, OwnerTable).Entry(Entry);
}

#undef LOCTEXT_NAMESPACE
