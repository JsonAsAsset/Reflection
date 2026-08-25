/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Font/FontImporter.h"

#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Engine/EngineUtilities.h"

UObject* IFontImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UFont>(GetPackage(), UFont::StaticClass(), *GetAssetName(), RF_Public | RF_Standalone));
}

bool IFontImporter::Import() {
	UFont* Font = Create<UFont>();

	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), Font);

	/* Worth saying which of the two it is, since an offline font that arrived without its texture
	 * pages and a runtime font that arrived without its faces both draw nothing and look alike */
	const int32 Entries = Font->CompositeFont.DefaultTypeface.Fonts.Num();

	if (Font->FontCacheType == EFontCacheType::Runtime) {
		int32 Resolved = 0;

		for (const FTypefaceEntry& Entry : Font->CompositeFont.DefaultTypeface.Fonts) {
			if (Entry.Font.GetFontFaceAsset() != nullptr) Resolved++;
		}

		UE_LOG(LogReflection, Display, TEXT("\"%s\" is a runtime font naming %d typeface(s), %d of them resolved"), *GetAssetName(), Entries, Resolved);

		if (Entries > 0 && Resolved == 0) {
			FImportIssues::Report(
				EImportIssue::Data,
				TEXT("The font names typefaces it hasn't got"),
				FString::Printf(
					TEXT("'%s' lists %d typeface(s) and none of them came through as a font face asset, so it draws nothing."),
					*GetAssetName(), Entries)
			);
		}
	} else {
		UE_LOG(LogReflection, Display, TEXT("\"%s\" is an offline font with %d texture page(s) and %d character(s)"), *GetAssetName(), Font->Textures.Num(), Font->Characters.Num());
	}

	Font->PostEditChange();

	return OnAssetCreation(Font);
}
