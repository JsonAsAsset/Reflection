/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Texture/TextureFormats.h"

#include "detex.h"
#include "nvimage/DirectDrawSurface.h"
#include "nvimage/Image.h"
#include "ThirdParty/NVTT/ThirdParty/NVTTStream.h"

int64 FTextureSourceLayout::GetEncodedSliceSize(const int32 SizeX, const int32 SizeY) const {
	const int64 BlocksX = (SizeX + BlockDim - 1) / BlockDim;
	const int64 BlocksY = (SizeY + BlockDim - 1) / BlockDim;

	return BlocksX * BlocksY * BytesPerBlock;
}

int64 FTextureSourceLayout::GetDecodedSliceSize(const int32 SizeX, const int32 SizeY) const {
	return static_cast<int64>(SizeX) * SizeY * BytesPerVoxel;
}

EPixelFormat FTextureFormats::FromName(const FString& Name) {
	if (Name.IsEmpty()) {
		return PF_Unknown;
	}

	const int64 Value = UTexture::GetPixelFormatEnum()->GetValueByNameString(Name);
	if (Value == INDEX_NONE) {
		return PF_Unknown;
	}

	return static_cast<EPixelFormat>(Value);
}

bool FTextureFormats::GetLayout(const EPixelFormat Format, FTextureSourceLayout& OutLayout) {
	OutLayout = FTextureSourceLayout();

	switch (Format) {
		/* Uncompressed, straight copy into the source buffer */
		case PF_B8G8R8A8:
			return true;

		case PF_FloatRGBA:
			OutLayout.SourceFormat = TSF_RGBA16F;
			OutLayout.BytesPerVoxel = 8;
			OutLayout.BytesPerBlock = 8;
			return true;

	/* TSF_G16 arrived in 4.25, before that a 16 bit grey source has no matching source format */
#if !UE4_24_BELOW
		case PF_G16:
			OutLayout.SourceFormat = TSF_G16;
			OutLayout.BytesPerVoxel = 2;
			OutLayout.BytesPerBlock = 2;
			return true;
#endif

		/* Grey, one byte in and replicated out to BGRA by the decode */
		case PF_G8:
			OutLayout.BytesPerBlock = 1;
			OutLayout.RequiresDecode = true;
			return true;

		/* Block compressed, 8 bytes per 4x4 block */
		case PF_DXT1:
		case PF_BC4:
			OutLayout.BlockDim = 4;
			OutLayout.BytesPerBlock = 8;
			OutLayout.RequiresDecode = true;
			return true;

		/* Block compressed, 16 bytes per 4x4 block.
		 * BC6H is HDR and decodes down to BGRA8 here, which clips it, but detex has no float
		 * output wired up yet and a clipped texture beats no texture. */
		case PF_DXT3:
		case PF_DXT5:
		case PF_BC5:
		case PF_BC6H:
		case PF_BC7:
			OutLayout.BlockDim = 4;
			OutLayout.BytesPerBlock = 16;
			OutLayout.RequiresDecode = true;
			return true;

		default:
			return false;
	}
}

void FTextureFormats::Decode(const uint8* Data, uint8* OutData, const int32 SizeX, const int32 SizeY, const int64 DecodedSize, const EPixelFormat Format) {
#if PLATFORM_WINDOWS
	switch (Format) {
		case PF_BC7: {
			detexTexture Texture;
			Texture.data = const_cast<uint8*>(Data);
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
			Texture.data = const_cast<uint8*>(Data);
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
			Texture.data = const_cast<uint8*>(Data);
			Texture.format = DETEX_TEXTURE_FORMAT_BC3;
			Texture.width = SizeX;
			Texture.height = SizeY;
			Texture.width_in_blocks = SizeX / 4;
			Texture.height_in_blocks = SizeY / 4;

			detexDecompressTextureLinear(&Texture, OutData, DETEX_PIXEL_FORMAT_BGRA8);
		}
		break;

		/* Gray/Grey, not Green, typically actually uses a red format with replication of R to RGB */
		case PF_G8: {
			const uint8* Source = Data;
			uint8* Destination = OutData;

			for (int32 Index = 0; Index < SizeX * SizeY; Index++) {
				const uint8 Grey = *Source++;

				*Destination++ = Grey;
				*Destination++ = Grey;
				*Destination++ = Grey;
				*Destination++ = 255;
			}
		}
		break;

		/* FloatRGBA: 16F
		 * G16: Gray/Grey like G8 */
		case PF_B8G8R8A8:
		case PF_FloatRGBA:
		case PF_G16: {
			FMemory::Memcpy(OutData, Data, DecodedSize);
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
			Header.setDepth(1);
			Header.setNormalFlag(Format == PF_BC5);
			DecodeDDS(Data, SizeX, SizeY, 1, Header, Image);

			FMemory::Memcpy(OutData, Image.pixels(), DecodedSize);
		}
		break;
	}
#endif
}
