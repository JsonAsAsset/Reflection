/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Font/FontFaceImporter.h"

#include "Engine/FontFace.h"
#include "Engine/EngineUtilities.h"

#include "Modules/Cloud/Cloud.h"

UObject* IFontFaceImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UFontFace>(GetPackage(), UFontFace::StaticClass(), *GetAssetName(), RF_Public | RF_Standalone));
}

bool IFontFaceImporter::Import() {
	UFontFace* FontFace = Create<UFontFace>();

	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), FontFace);

	/* Asked for by the path the game cooked it under, the way the rest of the Cloud tools do */
	FString FetchPath = GetAssetExport()->HasField(TEXT("Package"))
		? GetAssetExport()->GetStringField(TEXT("Package"))
		: FString();

	if (FetchPath.IsEmpty()) {
		FetchPath = GetPackage()->GetPathName();

		FRRedirects::Reverse(FetchPath);
	}

	const FBlockingRequestScope BlockingScope(FText::Format(
		NSLOCTEXT("Reflection", "FetchingFontFace", "Reading the typeface for {0}"),
		FText::FromString(FetchPath)
	));

	const TArray<uint8> Typeface = Cloud::Export::GetFontFaceBlocking(FetchPath);

	if (Typeface.Num() == 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The font face has no typeface"),
			FString::Printf(
				TEXT("'%s' came back without the font itself, so the asset carries its settings and draws nothing. A face saved with its data kept beside it rather than inline is the usual reason."),
				*GetAssetName()
			)
		);

		return OnAssetCreation(FontFace);
	}

	/* The same call the engine's own font importer makes, so the face ends up holding its typeface
	 * the way one built in the editor would */
	FontFace->InitializeFromBulkData(FontFace->SourceFilename, FontFace->Hinting, Typeface.GetData(), Typeface.Num());

	UE_LOG(LogReflection, Display, TEXT("\"%s\" carries %d byte(s) of typeface"), *GetAssetName(), Typeface.Num());

	FontFace->PostEditChange();

	return OnAssetCreation(FontFace);
}
