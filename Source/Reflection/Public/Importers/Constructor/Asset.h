/* Copyright Reflection Contributors 2024-2026 */

#pragma once

struct REFLECTION_API FAssetUtilities {
public:
	/* Creates a UPackage to create assets in the Content Browser. */
	static UPackage* CreateAssetPackage(const FString& FullPath);
	static UPackage* CreateAssetPackage(const FString& Name, const FString& OutputPath);
	static UPackage* CreateAssetPackage(const FString& Name, const FString& OutputPath, FString& FailureReason);
	
public:
	/* Off, a reference to an asset the project doesn't have is left null rather than reflected.
	 * A skeleton is still fetched: a skeletal mesh has nothing to build against without one. */
	static inline bool bImportReferences = true;

	/* Importing assets from Cloud */
	template <class T = UObject>
	static bool ConstructAsset(const FString& Path, const FString& RealPath, const FString& Type, TObjectPtr<T>& OutObject, bool& bSuccess);
	
	/* Kept for the tools already calling it, see FTextureImport for the rest of the texture path */
	static bool Fast_Construct_TypeTexture(const TSharedPtr<FJsonObject>& JsonExport, const FString& Path, const FString& Type, TArray<uint8> Data, UTexture*& OutTexture);
};
