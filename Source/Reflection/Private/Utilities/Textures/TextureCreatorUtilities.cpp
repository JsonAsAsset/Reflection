/* Copyright Reflection Contributors 2024-2026 */

#include "Utilities/Textures/TextureCreatorUtilities.h"

#include "detex.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureCube.h"
#include "Engine/VolumeTexture.h"
#include "Factories/TextureFactory.h"
#include "Factories/TextureRenderTargetFactoryNew.h"
#include "nvimage/DirectDrawSurface.h"
#include "nvimage/Image.h"
#include "Settings/Runtime.h"
#include "ThirdParty/NVTT/ThirdParty/NVTTStream.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonUtilities.h"

template bool FTextureCreatorUtilities::CreateTexture<UTexture2D>(UTexture*&, TArray<uint8>&, const TSharedPtr<FJsonObject>&);
template bool FTextureCreatorUtilities::CreateTexture<UTextureLightProfile>(UTexture*&, TArray<uint8>&, const TSharedPtr<FJsonObject>&);

template <typename T>
bool FTextureCreatorUtilities::CreateTexture(UTexture*& OutTexture, TArray<uint8>& Data, const TSharedPtr<FJsonObject>& Properties) {
	UTexture2D* Texture2D;

	if (IsOctetStreamEnabled()) {
		Texture2D = NewObject<T>(Package, T::StaticClass(), *AssetName, RF_Standalone | RF_Public);
	} else {
		UTextureFactory* TextureFactory = NewObject<UTextureFactory>();
		TextureFactory->AddToRoot();
		TextureFactory->SuppressImportOverwriteDialog();

		const uint8* ImageData = Data.GetData();
		Texture2D = Cast<T>(TextureFactory->FactoryCreateBinary(T::StaticClass(), Package, *AssetName, RF_Standalone | RF_Public, nullptr,
			*FPaths::GetExtension(AssetName + ".png").ToLower(), ImageData, ImageData + Data.Num(), GWarn));
	}

	if (Texture2D == nullptr) return false;

	DeserializeTexture2D(Texture2D, Properties->GetObjectField(TEXT("Properties")));

	if (!IsOctetStreamEnabled()
		&& GReflectionRuntime.IsOlderUE4Target()
		&&
		(
			Texture2D->LODGroup == TEXTUREGROUP_CharacterNormalMap
			|| Texture2D->LODGroup == TEXTUREGROUP_VehicleNormalMap
			|| Texture2D->LODGroup == TEXTUREGROUP_WorldNormalMap
			|| Texture2D->CompressionSettings == TC_Normalmap
		)) {

		/* Normal Map Swizzle | Older Unreal Engine 4 builds */
		const int32 Width = Texture2D->Source.GetSizeX();
		const int32 Height = Texture2D->Source.GetSizeY();

		uint8* Src = Texture2D->Source.LockMip(0);
		for (int32 i = 0, n = Width * Height; i < n; i++)
		{
			uint8* p = Src + i * 4;
			const uint8 R = p[2], G = p[1], A = p[3];
			p[0] = R; p[1] = G; p[2] = A; p[3] = 255;
		}
		
		Texture2D->Source.UnlockMip(0);
	}

	else {
		SetPlatformData(Texture2D, new FTexturePlatformData());
	}

	FTexturePlatformData* PlatformData = GetPlatformData(Texture2D);

	if (const TArray<TSharedPtr<FJsonValue>>* TextureMipsPtr; Properties->TryGetArrayField(TEXT("Mips"), TextureMipsPtr)) {
		const auto TextureMips = *TextureMipsPtr;
	
		if (TextureMips.Num() == 1) {
			Texture2D->MipGenSettings = TMGS_NoMipmaps;
		}
	}
	
	if (IsOctetStreamEnabled()) {
		DeserializeTexturePlatformData(Texture2D, Data, *PlatformData, Properties);
	}

	OutTexture = Texture2D;

	return false;
}

bool FTextureCreatorUtilities::IsOctetStreamEnabled() const {
#if PLATFORM_LINUX
	return false;
#else
	return UseOctetStream;
#endif
}

bool FTextureCreatorUtilities::CreateTextureCube(UTexture*& OutTextureCube, const TArray<uint8>& Data, const TSharedPtr<FJsonObject>& Properties) const {
	UTextureCube* TextureCube = NewObject<UTextureCube>(Package, UTextureCube::StaticClass(), *AssetName, RF_Public | RF_Standalone);

	SetPlatformData(TextureCube, new FTexturePlatformData());
	
	DeserializeTexture(TextureCube, Properties);

	FTexturePlatformData* PlatformData = GetPlatformData(TextureCube);

	const int SizeX = Properties->GetNumberField(TEXT("SizeX"));
	const int SizeY = Properties->GetNumberField(TEXT("SizeY")) / 6;

	FString PixelFormat;
	if (Properties->TryGetStringField(TEXT("PixelFormat"), PixelFormat)) {
		PlatformData->PixelFormat = static_cast<EPixelFormat>(TextureCube->GetPixelFormatEnum()->GetValueByNameString(PixelFormat));
	}

	int Size = SizeX * SizeY * (PlatformData->PixelFormat == PF_BC6H ? 16 : 4);
	if (PlatformData->PixelFormat == PF_FloatRGBA) Size = Data.Num();
	uint8* DecompressedData = static_cast<uint8*>(FMemory::Malloc(Size));

	ETextureSourceFormat Format = TSF_BGRA8;
	if (TextureCube->CompressionSettings == TC_HDR) Format = TSF_RGBA16F;
	TextureCube->Source.Init(SizeX, SizeY, 1, 1, Format);
	uint8_t* Dest = TextureCube->Source.LockMip(0);
	FMemory::Memcpy(Dest, DecompressedData, Size);
	TextureCube->Source.UnlockMip(0);

	TextureCube->PostEditChange();

	if (TextureCube) {
		OutTextureCube = TextureCube;
		return true;
	}

	return false;
}

bool FTextureCreatorUtilities::CreateVolumeTexture(UTexture*& OutVolumeTexture, TArray<uint8>& Data, const TSharedPtr<FJsonObject>& Properties) const {
	UVolumeTexture* VolumeTexture = NewObject<UVolumeTexture>(Package, UVolumeTexture::StaticClass(), *AssetName, RF_Public | RF_Standalone);

	DeserializeTexture(VolumeTexture, Properties);

	SetPlatformData(VolumeTexture, new FTexturePlatformData());
	FString PixelFormat;

	FTexturePlatformData* PlatformData = GetPlatformData(VolumeTexture);

	if (PlatformData != nullptr) {
		if (Properties->TryGetStringField(TEXT("PixelFormat"), PixelFormat)) {
			PlatformData->PixelFormat = static_cast<EPixelFormat>(VolumeTexture->GetPixelFormatEnum()->GetValueByNameString(PixelFormat));
		}

	}

	DeserializeTexture(VolumeTexture, Properties);

	/* const int SizeX = Properties->GetNumberField(TEXT("SizeX"));
	const int SizeY = Properties->GetNumberField(TEXT("SizeY"));
	const int SizeZ = Properties->GetNumberField(TEXT("SizeZ")); Need to add the property */
	/*constexpr int SizeZ = 1;
	int Size = SizeX * SizeY * SizeZ;*/

	/* Decompression */
	/*uint8* DecompressedData = static_cast<uint8*>(FMemory::Malloc(Size));
	GetDecompressedTextureData(Data.GetData(), DecompressedData, SizeX, SizeY, SizeZ, Size, PlatformData->PixelFormat);

	VolumeTexture->Source.Init(SizeX, SizeY, SizeZ, 1, TSF_BGRA8);

	uint8_t* Dest = VolumeTexture->Source.LockMip(0);
	FMemory::Memcpy(Dest, DecompressedData, Size);
	VolumeTexture->Source.UnlockMip(0);
	VolumeTexture->UpdateResource();*/

	if (VolumeTexture) {
		OutVolumeTexture = VolumeTexture;
		return true;
	}

	return false;
}

bool FTextureCreatorUtilities::CreateRenderTarget2D(UTexture*& OutRenderTarget2D, const TSharedPtr<FJsonObject>& Properties) const {
	UTextureRenderTargetFactoryNew* TextureFactory = NewObject<UTextureRenderTargetFactoryNew>();
	TextureFactory->AddToRoot();
	UTextureRenderTarget2D* RenderTarget2D = Cast<UTextureRenderTarget2D>(TextureFactory->FactoryCreateNew(UTextureRenderTarget2D::StaticClass(), Package, *AssetName, RF_Standalone | RF_Public, nullptr, GWarn));

	DeserializeTexture(RenderTarget2D, Properties);

	int SizeX;
	if (Properties->TryGetNumberField(TEXT("SizeX"), SizeX)) RenderTarget2D->SizeX = SizeX;
	int SizeY;
	if (Properties->TryGetNumberField(TEXT("SizeY"), SizeY)) RenderTarget2D->SizeY = SizeY;

	FString AddressX;
	if (Properties->TryGetStringField(TEXT("AddressX"), AddressX)) RenderTarget2D->AddressX = static_cast<TextureAddress>(StaticEnum<TextureAddress>()->GetValueByNameString(AddressX));
	FString AddressY;
	if (Properties->TryGetStringField(TEXT("AddressY"), AddressY)) RenderTarget2D->AddressY = static_cast<TextureAddress>(StaticEnum<TextureAddress>()->GetValueByNameString(AddressY));
	FString RenderTargetFormat;
	if (Properties->TryGetStringField(TEXT("RenderTargetFormat"), RenderTargetFormat)) RenderTarget2D->RenderTargetFormat = static_cast<ETextureRenderTargetFormat>(StaticEnum<ETextureRenderTargetFormat>()->GetValueByNameString(RenderTargetFormat));

	bool bAutoGenerateMips;
	if (Properties->TryGetBoolField(TEXT("bAutoGenerateMips"), bAutoGenerateMips)) RenderTarget2D->bAutoGenerateMips = bAutoGenerateMips;
	if (bAutoGenerateMips) {
		FString MipsSamplerFilter;
		
		if (Properties->TryGetStringField(TEXT("MipsSamplerFilter"), MipsSamplerFilter))
			RenderTarget2D->MipsSamplerFilter = static_cast<TextureFilter>(StaticEnum<TextureFilter>()->GetValueByNameString(MipsSamplerFilter));
	}

	if (RenderTarget2D) {
		OutRenderTarget2D = RenderTarget2D;
		return true;
	}

	return false;
}

bool FTextureCreatorUtilities::DeserializeTexture2D(UTexture2D* InTexture2D, const TSharedPtr<FJsonObject>& Properties) const {
	if (InTexture2D == nullptr) return false;

	DeserializeTexture(InTexture2D, Properties);

	FString AddressX;
	FString AddressY;
	bool bHasBeenPaintedInEditor;

	if (Properties->TryGetStringField(TEXT("AddressX"), AddressX)) InTexture2D->AddressX = static_cast<TextureAddress>(StaticEnum<TextureAddress>()->GetValueByNameString(AddressX));
	if (Properties->TryGetStringField(TEXT("AddressY"), AddressY)) InTexture2D->AddressY = static_cast<TextureAddress>(StaticEnum<TextureAddress>()->GetValueByNameString(AddressY));
	if (Properties->TryGetBoolField(TEXT("bHasBeenPaintedInEditor"), bHasBeenPaintedInEditor)) InTexture2D->bHasBeenPaintedInEditor = bHasBeenPaintedInEditor;

	/* ~~~~~~~~~~~~~ Platform Data ~~~~~~~~~~~~~ */
	FTexturePlatformData* PlatformData = GetPlatformData(InTexture2D);
	
	int SizeX;
	int SizeY;
	uint32 PackedData;
	FString PixelFormat;

	if (Properties->TryGetNumberField(TEXT("SizeX"), SizeX)) PlatformData->SizeX = SizeX;
	if (Properties->TryGetNumberField(TEXT("SizeY"), SizeY)) PlatformData->SizeY = SizeY;
	if (Properties->TryGetNumberField(TEXT("PackedData"), PackedData)) PlatformData->PackedData = PackedData;
	if (Properties->TryGetStringField(TEXT("PixelFormat"), PixelFormat)) PlatformData->PixelFormat = static_cast<EPixelFormat>(InTexture2D->GetPixelFormatEnum()->GetValueByNameString(PixelFormat));

	int FirstResourceMemMip;
	int LevelIndex;
	
	if (Properties->TryGetNumberField(TEXT("FirstResourceMemMip"), FirstResourceMemMip)) InTexture2D->FirstResourceMemMip = FirstResourceMemMip;
	if (Properties->TryGetNumberField(TEXT("LevelIndex"), LevelIndex)) InTexture2D->LevelIndex = LevelIndex;

	return false;
}

bool FTextureCreatorUtilities::DeserializeTexture(UTexture* Texture, const TSharedPtr<FJsonObject>& Properties) const {
	if (Texture == nullptr) return false;

	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(Properties, {
		"ImportedSize",
		"LODBias"
	}), Texture);
	
	return false;
}

bool FTextureCreatorUtilities::DeserializeTexturePlatformData(UTexture* Texture, TArray<uint8>& Data, FTexturePlatformData& TexturePlatformData,
	const TSharedPtr<FJsonObject>& Properties)
{
	const int SizeX = Properties->GetNumberField(TEXT("SizeX"));
	const int SizeY = Properties->GetNumberField(TEXT("SizeY"));
	constexpr int SizeZ = 1;

	FString PixelFormat;
	if (Properties->TryGetStringField(TEXT("PixelFormat"), PixelFormat)) {
		TexturePlatformData.PixelFormat = static_cast<EPixelFormat>(Texture->GetPixelFormatEnum()->GetValueByNameString(PixelFormat));
	}

	int Size = SizeX * SizeY * (TexturePlatformData.PixelFormat == PF_BC6H ? 16 : 4);
	if (TexturePlatformData.PixelFormat == PF_B8G8R8A8 || TexturePlatformData.PixelFormat == PF_FloatRGBA || TexturePlatformData.PixelFormat == PF_G16) Size = Data.Num();
	uint8* DecompressedData = static_cast<uint8*>(FMemory::Malloc(Size));

	if (IsOctetStreamEnabled()) {
		GetDecompressedTextureData(Data.GetData(), DecompressedData, SizeX, SizeY, SizeZ, Size, TexturePlatformData.PixelFormat);
	} else {
		DecompressedData = Data.GetData();
	}

	ETextureSourceFormat Format = TSF_BGRA8;
	if (Texture->CompressionSettings == TC_HDR) Format = TSF_RGBA16F;
	if (TexturePlatformData.PixelFormat == PF_G16) Format = TSF_G16;
	Texture->Source.Init(SizeX, SizeY, 1, 1, Format);
	uint8_t* Dest = Texture->Source.LockMip(0);
	FMemory::Memcpy(Dest, DecompressedData, Size);
	Texture->Source.UnlockMip(0);

	if (Texture->LODGroup == 255) {
		Texture->LODGroup = TEXTUREGROUP_World;
	}

	Texture->UpdateResource();

	if (Texture && Texture->IsValidLowLevel() && Texture != nullptr) {
		return true;
	}
	
	return false;
}

void FTextureCreatorUtilities::GetDecompressedTextureData(uint8* Data, uint8*& OutData, const int SizeX, const int SizeY, const int SizeZ, const int TotalSize, const EPixelFormat Format) {
#if PLATFORM_WINDOWS
	/* NOTE: Not all formats are supported, feel free to add if needed. Formats may need other dependencies. */
	switch (Format) {
		case PF_BC7: {
			detexTexture Texture;
			Texture.data = Data;
			Texture.format = DETEX_TEXTURE_FORMAT_BPTC;
			Texture.width = SizeX;
			Texture.height = SizeY;
			Texture.width_in_blocks = SizeX / 4;
			Texture.height_in_blocks = SizeY / 4;

			detexDecompressTextureLinear(&Texture, OutData, DETEX_PIXEL_FORMAT_BGRA8);
		}
		break;

		case PF_BC6H: {
			detexTexture Texture;
			Texture.data = Data;
			Texture.format = DETEX_TEXTURE_FORMAT_BPTC_FLOAT;
			Texture.width = SizeX;
			Texture.height = SizeY;
			Texture.width_in_blocks = SizeX / 4;
			Texture.height_in_blocks = SizeY / 4;

			detexDecompressTextureLinear(&Texture, OutData, DETEX_PIXEL_FORMAT_BGRA8);
		}
		break;

		case PF_DXT5: {
			detexTexture Texture;
			{
				Texture.data = Data;
				Texture.format = DETEX_TEXTURE_FORMAT_BC3;
				Texture.width = SizeX;
				Texture.height = SizeY;
				Texture.width_in_blocks = SizeX / 4;
				Texture.height_in_blocks = SizeY / 4;
			}

			detexDecompressTextureLinear(&Texture, OutData, DETEX_PIXEL_FORMAT_BGRA8);
		}
		break;

		/* Gray/Grey, not Green, typically actually uses a red format with replication of R to RGB*/
		case PF_G8: {
			const uint8* s = Data;
			uint8* d = OutData;

			for (int i = 0; i < SizeX * SizeY; i++) {
				const uint8 b = *s++;
				*d++ = b;
				*d++ = b;
				*d++ = b;
				*d++ = 255;
			}
		}
		break;

		/*
		 * FloatRGBA: 16F
		 * G16: Gray/Grey like G8
		*/
		case PF_B8G8R8A8:
		case PF_FloatRGBA:
		case PF_G16: {
			FMemory::Memcpy(OutData, Data, TotalSize);
		}
		break;

		default: {
			nv::DDSHeader Header;
			nv::Image Image;

			uint FourCC;
			switch (Format) {
			case PF_BC4:
				FourCC = FOURCC_ATI1;
				break;
			case PF_BC5:
				FourCC = FOURCC_ATI2;
				break;
			case PF_DXT1:
				FourCC = FOURCC_DXT1;
				break;
			case PF_DXT3:
				FourCC = FOURCC_DXT3;
				break;
			default: FourCC = 0;
			}

			Header.setFourCC(FourCC);
			Header.setWidth(SizeX);
			Header.setHeight(SizeY);
			Header.setDepth(SizeZ);
			Header.setNormalFlag(Format == PF_BC5);
			DecodeDDS(Data, SizeX, SizeY, SizeZ, Header, Image);

			FMemory::Memcpy(OutData, Image.pixels(), TotalSize);
		}
		break;
	}
#endif
}
