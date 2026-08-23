/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Types/Texture/TextureFormats.h"
#include "Serializers/PropertySerializer.h"
#include "Engine/Texture2D.h"
#include "Dom/JsonObject.h"

/* The size and format an export was cooked at, once the stacked payload has been unfolded */
struct FTextureCookedLayout {
	int32 SizeX = 0;

	/* One slice, not the stacked height the export reports */
	int32 SizeY = 0;

	/* Slices stacked into the payload: 1 for a flat texture, 6 for a cube, the depth for a volume */
	int32 SizeZ = 1;

	/* A volume loses depth with every mip, a cube keeps its six faces all the way down */
	bool SlicesHalvePerMip = false;

	EPixelFormat PixelFormat = PF_Unknown;
	FTextureSourceLayout Source;

	/* Bytes the whole payload takes up at this size */
	int64 GetEncodedSize() const;

	/* Steps down to the next mip, false once there is nothing left to step down to */
	bool DropMip();
};

/* Builds one texture asset out of a Cloud export.
 *
 * Every creator takes the export object itself rather than its properties, because a texture is
 * described at both levels: the UPROPERTYs sit under "Properties", while the cooked size, pixel
 * format and mip description sit on the export next to it. */
struct FTextureCreator {
public:
	FTextureCreator(const FString& AssetName, const FString& FilePath, UPackage* Package, const bool UseRawMipData)
		: UseRawMipData(UseRawMipData), AssetName(AssetName), FilePath(FilePath), Package(Package)
	{
		PropertySerializer = NewObject<UPropertySerializer>();
		ObjectSerializer = NewObject<UObjectSerializer>();

		ObjectSerializer->SetPropertySerializer(PropertySerializer);
	}

	/* Builds whichever texture class Type names, false when it names none of them */
	bool Create(const FString& Type, const TSharedPtr<FJsonObject>& Export, TArray<uint8>& Data, UTexture*& OutTexture);

	/* Whether the payload holds raw mip bytes rather than an encoded image */
	bool IsRawMipData() const;

private:
	/* Complains on behalf of the classes that can't be rebuilt from an encoded image */
	bool RequireRawMipData(const TCHAR* What) const;

	/* Texture2D and the classes deriving from it, either handed to the texture factory or rebuilt by hand */
	template <class T = UTexture2D>
	bool CreateTexture2D(UTexture*& OutTexture, TArray<uint8>& Data, const TSharedPtr<FJsonObject>& Export);

	/* UTexture2DArray is 4.24 and up */
#if !UE4_23_BELOW
	bool CreateTexture2DArray(UTexture*& OutTexture2DArray, TArray<uint8>& Data, const TSharedPtr<FJsonObject>& Export);
#endif
	bool CreateTextureCube(UTexture*& OutTextureCube, TArray<uint8>& Data, const TSharedPtr<FJsonObject>& Export);
	bool CreateVolumeTexture(UTexture*& OutVolumeTexture, TArray<uint8>& Data, const TSharedPtr<FJsonObject>& Export);
	bool CreateRenderTarget2D(UTexture*& OutRenderTarget2D, const TSharedPtr<FJsonObject>& Export);

	/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Source data ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

	/* Reads back the size and pixel format the export was cooked at.
	 *
	 * Cloud stacks the slices of a cube or a volume vertically into one tall image, so SizeY comes
	 * back as SizeY * SizeZ and has to be unfolded. FallbackSlices covers Cloud builds too old to
	 * send a slice count: pass the count for classes that have a fixed one, or zero to work it out
	 * from the width on the assumption that the slices are square. */
	bool GetCookedLayout(const TSharedPtr<FJsonObject>& Export, int32 FallbackSlices, FTextureCookedLayout& OutCooked) const;

	/* The same, for classes whose slices sit one after another with SizeY left alone.
	 *
	 * A texture array is stored that way rather than stacked, so its height needs no unfolding and
	 * the count has to be read off the export instead of divided out of the height. */
	bool GetCookedArrayLayout(const TSharedPtr<FJsonObject>& Export, FTextureCookedLayout& OutCooked) const;

	/* Shared tail of both: pixel format, source layout and the sanity check over the lot */
	bool ReadCookedLayout(const TSharedPtr<FJsonObject>& Export, int32 SizeX, int32 SizeY, int32 Slices, FTextureCookedLayout& OutCooked) const;

	/* The slice count an export reports, from PackedData or the older standalone field */
	static int32 GetReportedSliceCount(const TSharedPtr<FJsonObject>& Export);

	/* Mip zero's own depth and the byte count it holds, both zero when it describes neither */
	static void GetFirstMipDescription(const TSharedPtr<FJsonObject>& Export, int32& OutSlices, int64& OutPayload);

	/* Decodes the raw first mip, slice by slice, into the texture's source data */
	bool BuildSourceFromRawMip(UTexture* Texture, const TArray<uint8>& Data, const FTextureCookedLayout& Cooked) const;

	/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Deserialization ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

	/* The UPROPERTYs shared by every texture class */
	void DeserializeTexture(UTexture* Texture, const TSharedPtr<FJsonObject>& Export) const;
	void DeserializeTexture2D(UTexture2D* Texture2D, const TSharedPtr<FJsonObject>& Export) const;

	/* Cloud only ever sends the top mip, so anything with just the one can't generate the rest */
	static bool HasSingleMip(const TSharedPtr<FJsonObject>& Export);

	/* The properties of an export, or an empty object when it carries none */
	static TSharedPtr<FJsonObject> GetProperties(const TSharedPtr<FJsonObject>& Export);

	bool UseRawMipData = true;

protected:
	FString AssetName;
	FString FilePath;
	UPackage* Package;
	UPropertySerializer* PropertySerializer;
	UObjectSerializer* ObjectSerializer;

public:
	FORCEINLINE UObjectSerializer* GetObjectSerializer() const { return ObjectSerializer; }
};
