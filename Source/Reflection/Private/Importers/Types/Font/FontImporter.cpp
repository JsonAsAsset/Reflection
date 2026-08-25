/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Font/FontImporter.h"

#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Engine/EngineUtilities.h"

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
