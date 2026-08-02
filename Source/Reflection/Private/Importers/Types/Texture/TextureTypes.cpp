/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Texture/TextureTypes.h"

const TArray<FString>& FTextureTypes::All() {
	static const TArray<FString> Types = {
		TEXT("Texture2D"),
		TEXT("TextureCube"),
		TEXT("VolumeTexture"),
		TEXT("TextureRenderTarget2D"),
		TEXT("TextureLightProfile"),
	};

	return Types;
}

bool FTextureTypes::IsSupported(const FString& Type) {
	return All().Contains(Type);
}

bool FTextureTypes::HasPixelPayload(const FString& Type) {
	return Type != TEXT("TextureRenderTarget2D");
}

bool FTextureTypes::RequiresRawMipData(const FString& Type, const bool bIsVectorDisplacementMap) {
#if PLATFORM_LINUX
	/* detex and nvtt are Windows only here, so Linux has to make do with the encoded image */
	return false;
#elif UE4_26_BELOW || UE5_5_BEYOND
	/* Outside 4.27 through 5.4 the texture factory doesn't line up, so everything goes raw */
	return true;
#else
	/* A cube is six faces and a volume is a stack of slices, neither of which survives being
	 * encoded down to one flat image. Light profiles are 16 bit and would lose their curve. */
	if (Type == TEXT("TextureCube")
	 || Type == TEXT("VolumeTexture")
	 || Type == TEXT("TextureLightProfile")) {
		return true;
	}

	return bIsVectorDisplacementMap;
#endif
}

bool FTextureTypes::IsVectorDisplacementMap(const TSharedPtr<FJsonObject>& Export) {
	const TSharedPtr<FJsonObject>* Properties;
	if (!Export.IsValid() || !Export->TryGetObjectField(TEXT("Properties"), Properties)) {
		return false;
	}

	FString CompressionSettings;
	if (!(*Properties)->TryGetStringField(TEXT("CompressionSettings"), CompressionSettings)) {
		return false;
	}

	return CompressionSettings.Contains(TEXT("TC_VectorDisplacementmap"))
		|| CompressionSettings.Contains(TEXT("TC_HDR"));
}
