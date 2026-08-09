/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Tools/ImportFromPath.h"

#include "Containers/Export.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Importers/Constructor/ImportReader.h"
#include "Importers/Types/Texture/TextureImporter.h"
#include "Importers/Types/Texture/TextureTypes.h"
#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Remote.h"
#include "Modules/UI/Reflect/SReflectPathsDialog.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/Dialog.h"

void TToolImportFromPath::Execute() {
	if (!Cloud::Status::IsOpened()) {
		SpawnPrompt("Reflection", "Cloud isn't running, so there is nowhere to fetch from.");

		return;
	}

	/* Nothing here goes through the reflect button, so this is where the project name gets fetched */
	if (!Cloud::EnsureMetadataBlocking()) {
		SpawnPrompt("Reflection", "Cloud didn't say which project it has loaded, so paths can't be resolved.");

		return;
	}

	TArray<FString> Paths;

	if (!SReflectPathsDialog::Open(Paths)) {
		return;
	}

	int32 Reflected = 0;
	const int32 Attempted = Paths.Num();

	FImportIssues::Begin();

	for (const FString& Path : Paths) {
		if (Import(Path)) {
			Reflected++;
		}
	}

	const bool Successful = Reflected == Attempted;

	AppendNotification(
		FText::FromString(Successful ? "Reflected From Path" : "Reflected With Failures"),
		FText::FromString(FString::Printf(TEXT("%d of %d"), Reflected, Attempted)),
		Successful ? 2.0f : 5.0f,
		Successful ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail,
		true,
		310.0f
	);
	FImportIssues::Finish();
}

bool TToolImportFromPath::Import(const FString& InPath) {
	/* Take whatever gets pasted: Type'/Game/Path/Asset.Asset', a path with an extension on it,
	 * or a bare package path */
	FString PackagePath = StripObjectOuter(InPath.TrimStartAndEnd());
	PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));

	/* Cloud cuts everything from the first dot, so an export name or an extension makes no
	 * difference to it, and the editor path below has to be cut the same way to match */
	int32 Dot;

	if (PackagePath.FindChar(TEXT('.'), Dot)) {
		LeftInline(PackagePath, Dot);
	}

	PackagePath.TrimStartAndEndInline();

	if (PackagePath.IsEmpty()) {
		return false;
	}

	/* Reached straight off a menu click, so nothing here has a continuation to hand a callback to.
	 * The scope is what keeps the editor drawn and cancellable while the requests run. */
	const FBlockingRequestScope BlockingScope(FText::Format(
		NSLOCTEXT("Reflection", "ReflectingPath", "Reflecting {0}"),
		FText::FromString(PackagePath)
	));

	const TSharedPtr<FJsonObject> Response = Cloud::Export::GetRawBlocking(PackagePath);
	if (Response == nullptr || !Response->HasField(TEXT("exports"))) {
		UE_LOG(LogReflection, Error, TEXT("Cloud has nothing at \"%s\""), *PackagePath);

		return false;
	}

	const TArray<TSharedPtr<FJsonValue>> Exports = Response->GetArrayField(TEXT("exports"));
	if (Exports.Num() == 0) {
		UE_LOG(LogReflection, Error, TEXT("Cloud has nothing at \"%s\""), *PackagePath);

		return false;
	}

	const TSharedPtr<FJsonObject> Export = Exports[0]->AsObject();

	FString Type;
	if (!Export.IsValid() || !Export->TryGetStringField(TEXT("Type"), Type)) {
		return false;
	}

	FString AssetName;
	PackagePath.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

	if (FTextureTypes::IsSupported(Type)) {
		UTexture* Texture = nullptr;

		return FTextureImport::FromCloud(PackagePath + "." + AssetName, PackagePath, Texture);
	}

	IImporter* OutImporter = nullptr;

	return IImportReader::ReadExportsAndImport(Exports, PackagePath, OutImporter, false, false);
}
