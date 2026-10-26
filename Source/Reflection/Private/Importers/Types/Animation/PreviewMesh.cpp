/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Animation/PreviewMesh.h"

#include "Engine/Log.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Interfaces/Interface_PreviewMeshProvider.h"

/* AssetRegistryModule.h only moved under an AssetRegistry/ folder later on */
#if UE4_25_BELOW
#include "AssetRegistryModule.h"
#else
#include "AssetRegistry/AssetRegistryModule.h"
#endif

bool FPreviewMesh::FromFolder(UObject* Asset, const USkeleton* Skeleton) {
	IInterface_PreviewMeshProvider* Previews = Cast<IInterface_PreviewMeshProvider>(Asset);

	if (Asset == nullptr || Previews == nullptr) return false;

	/* Already named one, which is the asset's own word and not this to overrule */
	if (Previews->GetPreviewMesh() != nullptr) return false;

	const FString Folder = FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName());

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> Beside;
	Registry.GetAssetsByPath(*Folder, Beside, /* bRecursive */ false);

	/* What the asset is rigged to, spelled the way the registry spells it */
	const FString Rigged = Skeleton != nullptr ? Skeleton->GetPathName() : FString();

	FAssetData Picked;

	for (const FAssetData& One : Beside) {
		if (One.AssetClassPath != USkeletalMesh::StaticClass()->GetClassPathName()) continue;

		/* Read off the registry rather than by loading, so a folder of meshes isn't loaded to
		 * choose one of them */
		if (!Rigged.IsEmpty()) {
			FString Built;

			if (!One.GetTagValue(TEXT("Skeleton"), Built)) continue;
			if (FPackageName::ExportTextPathToObjectPath(Built) != Rigged) continue;
		}

		/* By name where the folder keeps more than one, so the same folder always picks the same */
		if (!Picked.IsValid() || One.AssetName.LexicalLess(Picked.AssetName)) {
			Picked = One;
		}
	}

	if (!Picked.IsValid()) return false;

	USkeletalMesh* Mesh = Cast<USkeletalMesh>(Picked.GetAsset());

	if (Mesh == nullptr) return false;

	Previews->SetPreviewMesh(Mesh);

	UE_LOG(LogReflection, Display, TEXT("\"%s\" previews on \"%s\" from its own folder"), *Asset->GetName(), *Mesh->GetName());

	return true;
}
