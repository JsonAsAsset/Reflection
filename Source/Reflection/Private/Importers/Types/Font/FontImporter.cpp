/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Font/FontImporter.h"

#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Engine/EngineUtilities.h"

#include "Modules/Cloud/Cloud.h"

/* The typefaces a runtime font is drawn from.
 *
 * A font that keeps its faces inside itself holds them as subobjects, and those are built along
 * with the rest of it, out of the properties the export carries: a filename, a hinting mode, and
 * nothing to draw with. The typeface itself is bulk data the cook keeps beside the package, and
 * only the Cloud can hand it over.
 *
 * Asked for under the font's own path, because a face inside a font has no path of its own. What
 * comes back is the whole package's, and a font holding more than one face gets the same bytes for
 * each: the Cloud answers with the first face it finds and there is nothing in the export saying
 * which of them it was. Rare enough to be worth having the common case work. */
void IFontImporter::Repair(UObject* Asset) const {
	UFont* Font = Cast<UFont>(Asset);

	if (Font == nullptr || Font->FontCacheType != EFontCacheType::Runtime) return;

	TArray<UFontFace*> Wanting;

	for (const FTypefaceEntry& Entry : Font->CompositeFont.DefaultTypeface.Fonts) {
		UFontFace* Face = const_cast<UFontFace*>(Cast<UFontFace>(Entry.Font.GetFontFaceAsset()));

		/* Already holding one, which is a face that came in as an asset of its own */
		if (Face == nullptr || Face->GetFontFaceData()->HasData()) continue;

		Wanting.AddUnique(Face);
	}

	if (Wanting.Num() == 0) return;

	/* Read as a value rather than off the export itself, which is only handed out to something
	 * that might write to it, and this only reads */
	const FUObjectJsonValueExport Export = GetAssetAsValue();

	FString FetchPath = Export.Has(TEXT("Package")) ? Export.GetString(TEXT("Package")) : FString();

	if (FetchPath.IsEmpty()) {
		FetchPath = GetPackage()->GetPathName();

		FRRedirects::Reverse(FetchPath);
	}

	const FBlockingRequestScope BlockingScope(FText::Format(
		NSLOCTEXT("Reflection", "FetchingTypeface", "Reading the typeface for {0}"),
		FText::FromString(FetchPath)
	));

	const TArray<uint8> Typeface = Cloud::Export::GetFontFaceBlocking(FetchPath);

	if (Typeface.Num() == 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The font's faces have no typeface"),
			FString::Printf(
				TEXT("'%s' keeps %d face(s) inside itself and the typeface behind them didn't come back, so the font carries its settings and draws nothing."),
				*GetAssetName(), Wanting.Num())
		);

		return;
	}

	for (UFontFace* Face : Wanting) {
		/* The same call the engine's own font importer makes, so the face ends up holding its
		 * typeface the way one built in the editor would */
		Face->InitializeFromBulkData(Face->SourceFilename, Face->Hinting, Typeface.GetData(), Typeface.Num());

		Face->PostEditChange();
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" filled %d face(s) it holds with %d byte(s) of typeface"),
		*GetAssetName(), Wanting.Num(), Typeface.Num());
}

/* Worth saying which of the two it is, since an offline font that arrived without its texture pages
 * and a runtime font that arrived without its faces both draw nothing and look alike */
void IFontImporter::Validate(UObject* Asset) const {
	const UFont* Font = Cast<UFont>(Asset);
	if (Font == nullptr) return;

	if (Font->FontCacheType != EFontCacheType::Runtime) {
		UE_LOG(LogReflection, Display, TEXT("\"%s\" is an offline font with %d texture page(s) and %d character(s)"),
			*GetAssetName(), Font->Textures.Num(), Font->Characters.Num());

		return;
	}

	const int32 Entries = Font->CompositeFont.DefaultTypeface.Fonts.Num();

	const int32 Missing = FImportIssues::ReportIncomplete(
		Font->CompositeFont.DefaultTypeface.Fonts,
		[](const FTypefaceEntry& Entry) { return Entry.Font.GetFontFaceAsset() == nullptr; },
		TEXT("typefaces didn't come through as a font face"),
		FString::Printf(TEXT("'%s' names them and hasn't got them, so those draw nothing."), *GetAssetName())
	);

	UE_LOG(LogReflection, Display, TEXT("\"%s\" is a runtime font naming %d typeface(s), %d of them resolved"),
		*GetAssetName(), Entries, Entries - Missing);
}
