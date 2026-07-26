/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Serializers/PropertySerializer.h"
#include "Dom/JsonObject.h"

inline bool ShouldUseOctetStream(
	const FString& Type,
	const bool IsVectorDisplacementMap)
{
#if PLATFORM_LINUX
	return false;
#else
	
#if UE4_26_BELOW || UE5_5_BEYOND
	return true;
#else
	
	if (Type == "TextureLightProfile"
	 || Type == "TextureCube"
	 || Type == "VolumeTexture"
	 || Type == "TextureRenderTarget2D")
	{
		return true;
	}
	
	return IsVectorDisplacementMap;
#endif
#endif
}

struct FTextureCreatorUtilities {
public:
	FTextureCreatorUtilities(const FString& AssetName, const FString& FilePath, UPackage* Package, const bool UseOctetStream)
		: UseOctetStream(UseOctetStream), AssetName(AssetName), FilePath(FilePath), Package(Package)
	{
		PropertySerializer = NewObject<UPropertySerializer>();
		ObjectSerializer = NewObject<UObjectSerializer>();

		ObjectSerializer->SetPropertySerializer(PropertySerializer);
	}

	bool UseOctetStream = true;

	bool IsOctetStreamEnabled() const;

    template <class T = UObject>
	bool CreateTexture(UTexture*& OutTexture, TArray<uint8>& Data, const TSharedPtr<FJsonObject>& Properties);
	bool CreateTextureCube(UTexture*& OutTextureCube, const TArray<uint8>& Data, const TSharedPtr<FJsonObject>& Properties) const;
	bool CreateVolumeTexture(UTexture*& OutVolumeTexture, TArray<uint8>& Data, const TSharedPtr<FJsonObject>& Properties) const;
	bool CreateRenderTarget2D(UTexture*& OutRenderTarget2D, const TSharedPtr<FJsonObject>& Properties) const;

	/* Deserialization functions */
	bool DeserializeTexture2D(UTexture2D* InTexture2D, const TSharedPtr<FJsonObject>& Properties) const;
	bool DeserializeTexture(UTexture* Texture, const TSharedPtr<FJsonObject>& Properties) const;
	bool DeserializeTexturePlatformData(UTexture* Texture, TArray<uint8>& Data, FTexturePlatformData& TexturePlatformData, const TSharedPtr<FJsonObject>& Properties);

private:
	static void GetDecompressedTextureData(uint8* Data, uint8*& OutData, const int SizeX, const int SizeY, const int SizeZ, const int TotalSize, const EPixelFormat Format);

protected:
	FString AssetName;
	FString FilePath;
	UPackage* Package;
	UPropertySerializer* PropertySerializer;
	UObjectSerializer* ObjectSerializer;

public:
	FORCEINLINE UObjectSerializer* GetObjectSerializer() const { return ObjectSerializer; }
};
