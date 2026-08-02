/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Engine/Compatibility.h"
#include "Engine/Texture.h"
#include "PixelFormat.h"

/* How a cooked pixel format unpacks into texture source data.
 *
 * BlockDim is 1 for uncompressed formats, where BytesPerBlock is simply the bytes per pixel.
 * Block compressed pixels arrive as whole 4x4 blocks, and a volume holds one independent block
 * image per slice, so both the sizes below and the decode itself work a slice at a time. */
struct FTextureSourceLayout {
	/* What the rebuilt source data ends up as */
	ETextureSourceFormat SourceFormat = TSF_BGRA8;
	int32 BytesPerVoxel = 4;

	/* What arrives from Cloud */
	int32 BlockDim = 1;
	int32 BytesPerBlock = 4;
	bool RequiresDecode = false;

	/* Bytes one slice takes up as it arrives */
	int64 GetEncodedSliceSize(int32 SizeX, int32 SizeY) const;

	/* Bytes one slice takes up once decoded into texture source data */
	int64 GetDecodedSliceSize(int32 SizeX, int32 SizeY) const;
};

struct REFLECTION_API FTextureFormats {
	/* Resolves an export's PixelFormat field, PF_Unknown when the name isn't a pixel format */
	static EPixelFormat FromName(const FString& Name);

	/* False for formats with no source representation to rebuild from */
	static bool GetLayout(EPixelFormat Format, FTextureSourceLayout& OutLayout);

	/* Decodes one slice worth of pixels into OutData, which has to hold GetDecodedSliceSize bytes.
	 * Not every format is covered, feel free to add if needed. Some may need other dependencies. */
	static void Decode(const uint8* Data, uint8* OutData, int32 SizeX, int32 SizeY, int64 DecodedSize, EPixelFormat Format);
};
