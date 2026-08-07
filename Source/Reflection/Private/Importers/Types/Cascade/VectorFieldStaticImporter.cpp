/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Cascade/VectorFieldStaticImporter.h"

#include "VectorField/VectorFieldStatic.h"
#include "Math/Float16Color.h"

#include "Engine/Log.h"
#include "Modules/Cloud/Cloud.h"
#include "Settings/ReflectionSettings.h"

UObject* IVectorFieldStaticImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UVectorFieldStatic>(GetPackage(), UVectorFieldStatic::StaticClass(), *GetAssetName(), RF_Public | RF_Standalone));
}

bool IVectorFieldStaticImporter::Import() {
	UVectorFieldStatic* VectorField = Create<UVectorFieldStatic>();

	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), VectorField);

	/* The volume is sized off the grid, and the resource ensures on anything else, so a grid that
	 * didn't come through leaves nothing to build */
	const int64 VoxelCount = static_cast<int64>(VectorField->SizeX) * VectorField->SizeY * VectorField->SizeZ;

	if (VoxelCount <= 0 || VoxelCount > static_cast<int64>(MAX_int32) / static_cast<int64>(sizeof(FFloat16Color))) {
		UE_LOG(LogReflection, Error, TEXT("\"%s\" has an unusable vector field grid of %dx%dx%d."), *GetAssetName(), VectorField->SizeX, VectorField->SizeY, VectorField->SizeZ);

		return false;
	}

	const int32 VolumeSize = static_cast<int32>(VoxelCount * sizeof(FFloat16Color));

	TArray<uint8> VolumeData;

	if (!DownloadVolumeData(VolumeSize, VolumeData)) {
		/* A field of zeroes still opens, and still resolves whatever referenced it. Leaving the
		 * bulk data empty instead would ensure on every resource build the asset ever does. */
		UE_LOG(LogReflection, Warning, TEXT("No vector data for \"%s\", reflected as an empty field."), *GetAssetName());

		VolumeData.SetNumZeroed(VolumeSize);
	}

	VectorField->SourceData.Lock(LOCK_READ_WRITE); {
		void* Destination = VectorField->SourceData.Realloc(VolumeSize);

		FMemory::Memcpy(Destination, VolumeData.GetData(), VolumeSize);
	}
	VectorField->SourceData.Unlock();

	/* The resource is initialized out of HandleAssetCreation, which is what reads the above */
	return OnAssetCreation(VectorField);
}

bool IVectorFieldStaticImporter::DownloadVolumeData(const int32 ExpectedSize, TArray<uint8>& OutData) const {
	const UReflectionSettings* Settings = GetSettings();

	if (!Settings->EnableCloudServer) {
		return false;
	}

	FString FetchPath = GetPackage()->GetPathName(); {
		FRRedirects::Reverse(FetchPath);
	}

	if (!Cloud::Export::GetBinaryBlocking(FetchPath, TEXT("application/octet-stream"), OutData)) {
		return false;
	}

	/* Cooked packages carry the volume inline, so a short read is a different asset or a Cloud
	 * that packed it some other way, not a field worth keeping */
	if (OutData.Num() != ExpectedSize) {
		UE_LOG(LogReflection, Warning, TEXT("Vector data for \"%s\" is %d bytes, expected %d."), *GetAssetName(), OutData.Num(), ExpectedSize);

		return false;
	}

	return true;
}
