/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Tools/ImportFromPath.h"

#include "Containers/Export.h"
#include "Importers/Constructor/ImportReader.h"
#include "Importers/Types/Texture/TextureImporter.h"
#include "Importers/Types/Texture/TextureTypes.h"
#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Remote.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/AssetPaths.h"
#include "Utilities/Dialog.h"

void TToolImportFromPath::Execute() {
	if (!Cloud::Status::IsOpened()) {
		SpawnPrompt("Reflect From Path", "Cloud isn't running, so there is nowhere to fetch from.");

		return;
	}

	/* The path is usually already on the clipboard, straight out of the asset it was copied from */
	FString Paths = GetClipboard();
	if (!Paths.Contains(TEXT("/"))) {
		Paths.Empty();
	}

	/* The example writes itself out of whatever profile Cloud has loaded */
	const UReflectionSettings* Settings = GetSettings();

	const FString ProjectName = Settings->AssetSettings.ProjectName.IsEmpty()
		? TEXT("Project")
		: Settings->AssetSettings.ProjectName;

	if (!SpawnTextEntryPrompt(
		TEXT("Reflect From Path"),
		FString::Printf(TEXT("One asset path per line, as Cloud knows it:\n\n%s/Content/Path/To/Asset"), *ProjectName),
		Paths)) {
		return;
	}

	TArray<FString> Lines;
	Paths.ParseIntoArrayLines(Lines);

	int32 Reflected = 0;
	int32 Attempted = 0;

	for (const FString& Line : Lines) {
		if (Line.TrimStartAndEnd().IsEmpty()) continue;

		Attempted++;

		if (Import(Line)) {
			Reflected++;
		}
	}

	if (Attempted == 0) {
		return;
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
		PackagePath.LeftInline(Dot);
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

	/* Where it lands in the editor */
	const FString ObjectPath = ToEditorPackagePath(PackagePath);

	FString AssetName;
	ObjectPath.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

	if (FTextureTypes::IsSupported(Type)) {
		UTexture* Texture = nullptr;

		return FTextureImport::FromCloud(ObjectPath + "." + AssetName, PackagePath, Texture);
	}

	IImporter* OutImporter = nullptr;

	return IImportReader::ReadExportsAndImport(Exports, ObjectPath, OutImporter);
}
