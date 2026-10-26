/* Copyright Reflection Contributors 2024-2026 */

#include "ReflectImportCommandlet.h"

#include "FileHelpers.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"

#include "Importers/Constructor/ImportReader.h"
#include "Importers/Constructor/Importer.h"
#include "Importers/Types/Texture/TextureImporter.h"
#include "Importers/Types/Texture/TextureTypes.h"
#include "Importers/Types/World/LevelRead.h"
#include "Modules/Cloud/Cloud.h"
/* 5.0 gathered what a save is told into a header of its own */
#if ENGINE_UE5
#include "UObject/SavePackage.h"
#endif

DECLARE_LOG_CATEGORY_CLASS(LogReflectImport, All, All);

/* Everything the run left dirty, written out */
static int32 SaveDirtyPackages() {
	TArray<UPackage*> Dirty;
	FEditorFileUtils::GetDirtyContentPackages(Dirty);
	FEditorFileUtils::GetDirtyWorldPackages(Dirty);

	int32 Saved = 0;

	for (UPackage* One : Dirty) {
		const FString FileName = FPackageName::LongPackageNameToFilename(One->GetName(), FPackageName::GetAssetPackageExtension());

#if ENGINE_UE5
		/* A package saved empty once keeps PKG_ContainsNoAsset through every later save, which only ever
		 * adds it; the save sets it again if this one really holds no asset. */
		One->ClearPackageFlags(PKG_ContainsNoAsset);

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

		if (UPackage::SavePackage(One, nullptr, *FileName, SaveArgs)) {
#else
		if (UPackage::SavePackage(One, nullptr, RF_Public | RF_Standalone, *FileName)) {
#endif
			Saved++;
		} else {
			UE_LOG(LogReflectImport, Warning, TEXT("could not save %s"), *FileName);
		}
	}

	UE_LOG(LogReflectImport, Display, TEXT("%d package(s) saved"), Saved);

	return Saved;
}

/* The object path a cloud path names: <Project>/Content/A/B.uasset is /Game/A/B.B, and
 * .../<Plugin>/Content/A/B.uasset is /<Plugin>/A/B.B */
static FString ObjectPathFor(const FString& CloudPath) {
	FString Root;
	FString Rest;

	if (!CloudPath.Split(TEXT("/Content/"), &Root, &Rest)) {
		return FString();
	}

	Rest.RemoveFromEnd(TEXT(".uasset"));

	const FString Mount = Root.Contains(TEXT("/")) ? FPaths::GetCleanFilename(Root) : TEXT("Game");

	return TEXT("/") + Mount + TEXT("/") + Rest + TEXT(".") + FPaths::GetBaseFilename(Rest);
}

int32 UReflectImportCommandlet::Main(const FString& Params) {
	FString Path;

	if (!FParse::Value(*Params, TEXT("path="), Path) || Path.IsEmpty()) {
		UE_LOG(LogReflectImport, Error, TEXT("nothing to import: give it -path=<cloud asset path>"));

		return 1;
	}

	/* Who the exports belong to, which every path read out of them is said in terms of.
	 *
	 * The editor asks for this when it connects, so anything importing there already knows it. A
	 * commandlet connects to nothing and asks for one asset, and without the name none of the
	 * paths inside can be turned into the ones the editor uses. */
	if (!Cloud::EnsureMetadataBlocking()) {
		UE_LOG(LogReflectImport, Error, TEXT("the cloud would not say what project it is serving"));

		return 1;
	}

	TSharedPtr<FJsonObject> Response;

	/* Read out of a file rather than off the cloud.
	 *
	 * What the cloud gives for a path is the same every time, which is what makes it worth reading
	 * and no use for trying anything. Kept with -json and read back with -from, an export can be
	 * changed and the change put through the same importer: an asset whose class this engine has
	 * not got is reparented to one it has, and everything else about it is read the way it always
	 * would be. The path is still said, since that is where what comes out is written. */
	if (FString From; FParse::Value(*Params, TEXT("from="), From) && !From.IsEmpty()) {
		FString Held;

		if (!FFileHelper::LoadFileToString(Held, *From)) {
			UE_LOG(LogReflectImport, Error, TEXT("there is nothing to read at \"%s\""), *From);

			return 1;
		}

		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Held), Response) || !Response.IsValid()) {
			UE_LOG(LogReflectImport, Error, TEXT("\"%s\" is not something that reads as json"), *From);

			return 1;
		}

		UE_LOG(LogReflectImport, Display, TEXT("reading \"%s\" rather than asking the cloud"), *From);
	} else {
		Response = Cloud::Export::GetRawBlocking(Path);
	}

	if (!Response.IsValid() || !Response->HasField(TEXT("exports"))) {
		UE_LOG(LogReflectImport, Error, TEXT("there was nothing at \"%s\""), *Path);

		return 1;
	}

	const TArray<TSharedPtr<FJsonValue>> Exports = Response->GetArrayField(TEXT("exports"));

	UE_LOG(LogReflectImport, Display, TEXT("importing \"%s\", %d export(s)"), *Path, Exports.Num());

	/* What came back, kept where it was asked for, since what the cloud gave is the only thing the
	 * result is worth checking against */
	if (FString Dump; FParse::Value(*Params, TEXT("json="), Dump) && !Dump.IsEmpty()) {
		FString Written;

		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Written);

		if (FJsonSerializer::Serialize(Response.ToSharedRef(), Writer) && FFileHelper::SaveStringToFile(Written, *Dump)) {
			UE_LOG(LogReflectImport, Display, TEXT("what the cloud gave back was kept at \"%s\""), *Dump);
		}
	}

	/* A level writes no package, so there is nothing below for it to say anything about */
	if (FLevelRead::Handles(Exports)) {
		FRLevelReadResult Result;

		if (!FLevelRead::FromCloud(Exports, Path, &Result)) {
			UE_LOG(LogReflectImport, Error, TEXT("nothing was read from \"%s\""), *Path);

			return 1;
		}

		UE_LOG(LogReflectImport, Display, TEXT("%d placed, %d missing (%d blueprint, %d class, %d refused), %d left out"),
			Result.Placed, Result.Missing(), Result.MissingBlueprints, Result.UnusableClasses, Result.Failed, Result.LeftOut);

		/* Said out loud: headless there is no window for the report to come up in */
		for (const TSharedPtr<FImportIssueAsset>& Asset : FImportIssues::GetAssets()) {
			UE_LOG(LogReflectImport, Display, TEXT("  %s"), *Asset->Name);

			for (const TSharedPtr<FImportIssue>& Issue : Asset->Issues) {
				UE_LOG(LogReflectImport, Display, TEXT("    [%s] %s%s%s"),
					*GetImportIssueText(Issue->Kind).ToString(),
					*Issue->Summary,
					Issue->Count > 1 ? *FString::Printf(TEXT(" (x%d)"), Issue->Count) : TEXT(""),
					Issue->Detail.IsEmpty() ? TEXT("") : *(TEXT(": ") + Issue->Detail));
			}
		}

		return 0;
	}

	/* No package import builds a texture: one is only ever made as the reference of something else.
	 * Asked for on its own, it is fetched the way that reference would fetch it */
	if (const TSharedPtr<FJsonObject> First = Exports.Num() > 0 ? Exports[0]->AsObject() : nullptr; First.IsValid() && FTextureTypes::IsSupported(First->GetStringField(TEXT("Type")))) {
		UTexture* Texture = nullptr;

		if (!FTextureImport::FromCloud(ObjectPathFor(Path), Path, Texture) || Texture == nullptr) {
			UE_LOG(LogReflectImport, Error, TEXT("nothing was made from \"%s\""), *Path);

			return 1;
		}

		SaveDirtyPackages();

		return 0;
	}

	IImporter* Importer = nullptr;

	const bool bImported = IImportReader::ReadExportsAndImport(Exports, Path, Importer, true, false);

	if (!bImported || Importer == nullptr) {
		UE_LOG(LogReflectImport, Error, TEXT("nothing was made from \"%s\""), *Path);

		return 1;
	}

	/* What the package ended up holding, and how much of any graph in it is wired */
	UPackage* Package = Importer->GetPackage();

	if (Package != nullptr) {
		TArray<UObject*> Held;
		GetObjectsWithOuter(Package, Held, true);

		TMap<FName, int32> Kinds;

		int32 Graphs = 0;
		int32 Nodes = 0;
		int32 Pins = 0;
		int32 Links = 0;

		for (UObject* Object : Held) {
			if (Object == nullptr) continue;

			Kinds.FindOrAdd(Object->GetClass()->GetFName())++;

			const UEdGraph* Graph = Cast<UEdGraph>(Object);

			if (Graph == nullptr) continue;

			Graphs++;

			for (const UEdGraphNode* Node : Graph->Nodes) {
				if (Node == nullptr) continue;

				Nodes++;

				for (const UEdGraphPin* Pin : Node->Pins) {
					if (Pin == nullptr) continue;

					Pins++;

					Links += Pin->LinkedTo.Num();
				}
			}
		}

		UE_LOG(LogReflectImport, Display, TEXT("\"%s\" holds %d object(s) of %d kind(s)"),
			*Package->GetName(), Held.Num(), Kinds.Num());

		Kinds.ValueSort([](const int32 A, const int32 B) { return A > B; });

		for (const TPair<FName, int32>& Kind : Kinds) {
			UE_LOG(LogReflectImport, Display, TEXT("    %4d  %s"), Kind.Value, *Kind.Key.ToString());
		}

		UE_LOG(LogReflectImport, Display, TEXT("%d graph(s), %d node(s), %d pin(s), %d link(s)"),
			Graphs, Nodes, Pins, Links);

		/* The same again over every node in the package rather than only the ones a graph lists, so
		 * a node a graph forgot shows up as the difference between the two */
		int32 Loose = 0;
		int32 LoosePins = 0;
		int32 LooseLinks = 0;

		for (UObject* Object : Held) {
			const UEdGraphNode* Node = Cast<UEdGraphNode>(Object);

			if (Node == nullptr) continue;

			Loose++;

			for (const UEdGraphPin* Pin : Node->Pins) {
				if (Pin == nullptr) continue;

				LoosePins++;

				LooseLinks += Pin->LinkedTo.Num();
			}
		}

		UE_LOG(LogReflectImport, Display, TEXT("every node: %d node(s), %d pin(s), %d link(s)"),
			Loose, LoosePins, LooseLinks);
	}

	SaveDirtyPackages();

	return 0;
}
