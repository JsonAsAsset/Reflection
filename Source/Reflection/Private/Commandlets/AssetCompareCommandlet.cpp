/* Copyright Reflection Contributors 2024-2026 */

#include "AssetCompareCommandlet.h"

#include "Containers/Export.h"
#include "Engine/Compatibility.h"
#include "Modules/Cloud/Cloud.h"
#include "Serializers/AssetCompare.h"
#include "Serializers/PropertySerializer.h"

#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogAssetCompare, Display, All);

namespace {
	/* The package path both sides name the asset by, out of whatever was pasted.
	 *
	 * Said the same way the toolbar says it, since the Cloud cuts everything from the first dot
	 * and an editor path has to be cut the same way to match. */
	FString PackagePathOf(const FString& InPath) {
		FString Held = StripObjectOuter(InPath.TrimStartAndEnd());
		Held.ReplaceInline(TEXT("\\"), TEXT("/"));

		if (int32 Dot; Held.FindChar(TEXT('.'), Dot)) Held = Held.Left(Dot);

		return Held.TrimStartAndEnd();
	}

	/* A package path as the object inside it, which is what loading wants */
	FString ObjectPathOf(const FString& PackagePath) {
		FString Leaf;

		if (!PackagePath.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
			Leaf = PackagePath;
		}

		return FString::Printf(TEXT("%s.%s"), *PackagePath, *Leaf);
	}

	/* The export the asset itself came from, which is the one named after it. Falls back to the
	 * first the Cloud listed, since an export list is served asset first. */
	TSharedPtr<FJsonObject> AssetExportIn(const TArray<TSharedPtr<FJsonValue>>& Exports, const FString& Named) {
		TSharedPtr<FJsonObject> First;

		for (const TSharedPtr<FJsonValue>& Value : Exports) {
			if (!Value.IsValid() || Value->Type != EJson::Object) continue;

			const TSharedPtr<FJsonObject> Export = Value->AsObject();

			if (!First.IsValid()) First = Export;

			if (FString Held; Export->TryGetStringField(TEXT("Name"), Held) && Held == Named) {
				return Export;
			}
		}

		return First;
	}
}

int32 UAssetCompareCommandlet::Main(const FString& Params) {
	FString Path;

	if (!FParse::Value(*Params, TEXT("path="), Path) || Path.IsEmpty()) {
		UE_LOG(LogAssetCompare, Error, TEXT("nothing to compare: give it -path=<cloud asset path>"));

		return 1;
	}

	const FString PackagePath = PackagePathOf(Path);

	if (PackagePath.IsEmpty()) {
		UE_LOG(LogAssetCompare, Error, TEXT("\"%s\" does not read as an asset path"), *Path);

		return 1;
	}

	TSharedPtr<FJsonObject> Response;

	/* Out of a file rather than off the Cloud, so a payload kept from one build can be held
	 * against what this project has now */
	if (FString From; FParse::Value(*Params, TEXT("from="), From) && !From.IsEmpty()) {
		FString Held;

		if (!FFileHelper::LoadFileToString(Held, *From)) {
			UE_LOG(LogAssetCompare, Error, TEXT("there is nothing to read at \"%s\""), *From);

			return 1;
		}

		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Held), Response) || !Response.IsValid()) {
			UE_LOG(LogAssetCompare, Error, TEXT("\"%s\" is not something that reads as json"), *From);

			return 1;
		}
	} else {
		/* Who the exports belong to, which every path inside them is said in terms of */
		if (!Cloud::EnsureMetadataBlocking()) {
			UE_LOG(LogAssetCompare, Error, TEXT("the cloud would not say what project it is serving"));

			return 1;
		}

		Response = Cloud::Export::GetRawBlocking(PackagePath);
	}

	if (!Response.IsValid() || !Response->HasField(TEXT("exports"))) {
		UE_LOG(LogAssetCompare, Error, TEXT("the cloud has nothing at \"%s\""), *PackagePath);

		return 1;
	}

	const TArray<TSharedPtr<FJsonValue>> Exports = Response->GetArrayField(TEXT("exports"));

	/* Ours. At the same path unless told otherwise, since that is where a reflect of this path
	 * would have put it. */
	FString Ours;

	if (!FParse::Value(*Params, TEXT("asset="), Ours) || Ours.IsEmpty()) Ours = PackagePath;

	const FString OurObjectPath = ObjectPathOf(PackagePathOf(Ours));

	UObject* Asset = LoadObject<UObject>(nullptr, *OurObjectPath);

	if (Asset == nullptr) {
		UE_LOG(LogAssetCompare, Error,
			TEXT("this project has nothing at \"%s\". Say which asset to compare with -asset=<path> if it landed elsewhere."),
			*OurObjectPath);

		return 1;
	}

	/* What an import would and would not read, so a property the serializer passes over is not
	 * reported as the asset being wrong */
	const UPropertySerializer* Serializer = NewObject<UPropertySerializer>();

	const bool bEverything = FParse::Param(*Params, TEXT("all"));

	UE_LOG(LogAssetCompare, Display, TEXT("=== %s against %s, %d export(s) served"),
		*PackagePath, *Asset->GetPathName(), Exports.Num());

	const TSharedPtr<FJsonObject> Theirs = AssetExportIn(Exports, Asset->GetName());

	FAssetCompareReport Report = CompareExportAgainstAsset(Theirs, Asset, Serializer);

	for (const FString& Line : DescribeAssetCompare(Report, bEverything)) {
		UE_LOG(LogAssetCompare, Display, TEXT("%s"), *Line);
	}

	int32 Differs = Report.Differs;

	/* Every other export the asset carries, matched to the object of that name inside our own
	 * package. A material's expressions and an emitter's modules are exports of their own, and
	 * what went wrong in an import is usually in one of them rather than on the asset. */
	if (FParse::Param(*Params, TEXT("exports"))) {
		UPackage* Package = Asset->GetOutermost();

		int32 Compared = 0;
		int32 Unmatched = 0;

		for (const TSharedPtr<FJsonValue>& Value : Exports) {
			if (!Value.IsValid() || Value->Type != EJson::Object) continue;

			const TSharedPtr<FJsonObject> Export = Value->AsObject();

			if (Export == Theirs) continue;

			FString Named;

			if (!Export->TryGetStringField(TEXT("Name"), Named) || Named.IsEmpty()) continue;

			UObject* Inside = FindObject<UObject>(Package, *Named);

			if (Inside == nullptr) {
				Unmatched++;

				continue;
			}

			FAssetCompareReport Under = CompareExportAgainstAsset(Export, Inside, Serializer);

			Compared++;
			Differs += Under.Differs;

			/* Only the ones with something to say, unless everything was asked for */
			if (Under.Differs > 0 || Under.Absent > 0 || bEverything) {
				for (const FString& Line : DescribeAssetCompare(Under, bEverything)) {
					UE_LOG(LogAssetCompare, Display, TEXT("%s"), *Line);
				}
			}
		}

		UE_LOG(LogAssetCompare, Display, TEXT("%d subobject(s) compared, %d the package has not got"), Compared, Unmatched);
	}

	UE_LOG(LogAssetCompare, Display, TEXT("%d propert%s differ"), Differs, Differs == 1 ? TEXT("y") : TEXT("ies"));

	return Differs > 0 ? 2 : 0;
}
