/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/Compatibility.h"
#include "Dom/JsonObject.h"

/*The texture classes Reflection can rebuild from a Cloud export, and how their pixels travel.
 *
 * Cloud serves a texture one of two ways. An encoded image is the cheap path: it goes through
 * UTextureFactory, which handles mips and compression on its own. It is also eight bit and two
 * dimensional, so anything carrying more depth or precision than that has to ask for the raw
 * bytes of the first mip instead and be rebuilt by hand. */
struct REFLECTION_API FTextureTypes {
	/* Every texture class with a creator behind it */
	static const TArray<FString>& All();

	static bool IsSupported(const FString& Type);

	/* Render targets are created empty, every other texture arrives with pixels */
	static bool HasPixelPayload(const FString& Type);

	/* Whether the pixels have to come down as raw mip bytes rather than an encoded image */
	static bool RequiresRawMipData(const FString& Type, bool bIsVectorDisplacementMap);

	/* Compression settings an encoded image can't carry, read straight off the export */
	static bool IsVectorDisplacementMap(const TSharedPtr<FJsonObject>& Export);
};
