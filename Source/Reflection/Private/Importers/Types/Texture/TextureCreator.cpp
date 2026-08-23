/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Texture/TextureCreator.h"

#include "Importers/Constructor/ImportIssues.h"

#include "Engine/TextureCube.h"
#include "Engine/TextureLightProfile.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/VolumeTexture.h"
#include "Factories/TextureFactory.h"
#include "Factories/TextureRenderTargetFactoryNew.h"
#include "Settings/Runtime.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"

bool FTextureCreator::Create(const FString& Type, const TSharedPtr<FJsonObject>& Export, TArray<uint8>& Data, UTexture*& OutTexture) {
	if (Type == TEXT("Texture2D")) {
		return CreateTexture2D<UTexture2D>(OutTexture, Data, Export);
	}

	if (Type == TEXT("TextureLightProfile")) {
		return CreateTexture2D<UTextureLightProfile>(OutTexture, Data, Export);
	}

#if !UE4_23_BELOW
	if (Type == TEXT("Texture2DArray")) {
		return CreateTexture2DArray(OutTexture, Data, Export);
	}
#endif

	if (Type == TEXT("TextureCube")) {
		return CreateTextureCube(OutTexture, Data, Export);
	}

	if (Type == TEXT("VolumeTexture")) {
		return CreateVolumeTexture(OutTexture, Data, Export);
	}

	if (Type == TEXT("TextureRenderTarget2D")) {
		return CreateRenderTarget2D(OutTexture, Export);
	}

	UE_LOG(LogReflection, Error, TEXT("\"%s\" is a %s, which has no texture creator"), *AssetName, *Type);

	return false;
}

bool FTextureCreator::IsRawMipData() const {
#if PLATFORM_LINUX
	return false;
#else
	return UseRawMipData;
#endif
}

bool FTextureCreator::RequireRawMipData(const TCHAR* What) const {
	if (IsRawMipData()) {
		return true;
	}

	/* An encoded image is one flat eight bit picture. There is no stack of slices left in it to
	 * rebuild from, so these classes have nothing to work with. */
	UE_LOG(LogReflection, Error, TEXT("%s can only be rebuilt from raw mip data, which \"%s\" didn't come down as"), What, *AssetName);

	return false;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Creators ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

template <typename T>
bool FTextureCreator::CreateTexture2D(UTexture*& OutTexture, TArray<uint8>& Data, const TSharedPtr<FJsonObject>& Export) {
	UTexture2D* Texture2D;

	if (IsRawMipData()) {
		Texture2D = NewObject<T>(Package, T::StaticClass(), *AssetName, RF_Standalone | RF_Public);
	} else {
		UTextureFactory* TextureFactory = NewObject<UTextureFactory>();
		TextureFactory->AddToRoot();
		TextureFactory->SuppressImportOverwriteDialog();

		const uint8* ImageData = Data.GetData();
		Texture2D = Cast<T>(TextureFactory->FactoryCreateBinary(T::StaticClass(), Package, *AssetName, RF_Standalone | RF_Public, nullptr,
			*FPaths::GetExtension(AssetName + ".png").ToLower(), ImageData, ImageData + Data.Num(), GWarn));
	}

	if (Texture2D == nullptr) {
		return false;
	}

	DeserializeTexture2D(Texture2D, Export);

	if (!IsRawMipData()
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

	if (HasSingleMip(Export)) {
		Texture2D->MipGenSettings = TMGS_NoMipmaps;
	}

	/* An encoded image comes out of the factory with its source already filled in, only raw mips
	 * have to be turned back into source data by hand */
	if (IsRawMipData()) {
		FTextureCookedLayout Cooked;
		if (!GetCookedLayout(Export, 1, Cooked)) {
			return false;
		}

		if (FTexturePlatformData* PlatformData = GetPlatformData(Texture2D)) {
			PlatformData->PixelFormat = Cooked.PixelFormat;
		}

		if (!BuildSourceFromRawMip(Texture2D, Data, Cooked)) {
			return false;
		}
	}

	OutTexture = Texture2D;

	return true;
}

#if !UE4_23_BELOW
bool FTextureCreator::CreateTexture2DArray(UTexture*& OutTexture2DArray, TArray<uint8>& Data, const TSharedPtr<FJsonObject>& Export) {
	if (!RequireRawMipData(TEXT("An array"))) {
		return false;
	}

	UTexture2DArray* Texture2DArray = NewObject<UTexture2DArray>(Package, UTexture2DArray::StaticClass(), *AssetName, RF_Public | RF_Standalone);

	DeserializeTexture(Texture2DArray, Export);
	SetPlatformData(Texture2DArray, new FTexturePlatformData());

	/* Slices sit one after another rather than stacked into the height */
	FTextureCookedLayout Cooked;
	if (!GetCookedArrayLayout(Export, Cooked)) {
		return false;
	}

	if (FTexturePlatformData* PlatformData = GetPlatformData(Texture2DArray)) {
		PlatformData->PixelFormat = Cooked.PixelFormat;
	}

	if (HasSingleMip(Export)) {
		Texture2DArray->MipGenSettings = TMGS_NoMipmaps;
	}

	if (!BuildSourceFromRawMip(Texture2DArray, Data, Cooked)) {
		return false;
	}

	OutTexture2DArray = Texture2DArray;

	return true;
}
#endif

bool FTextureCreator::CreateTextureCube(UTexture*& OutTextureCube, TArray<uint8>& Data, const TSharedPtr<FJsonObject>& Export) {
	if (!RequireRawMipData(TEXT("A cube"))) {
		return false;
	}

	UTextureCube* TextureCube = NewObject<UTextureCube>(Package, UTextureCube::StaticClass(), *AssetName, RF_Public | RF_Standalone);

	DeserializeTexture(TextureCube, Export);
	SetPlatformData(TextureCube, new FTexturePlatformData());

	/* Always the same six faces, stacked */
	FTextureCookedLayout Cooked;
	if (!GetCookedLayout(Export, 6, Cooked)) {
		return false;
	}

	if (FTexturePlatformData* PlatformData = GetPlatformData(TextureCube)) {
		PlatformData->PixelFormat = Cooked.PixelFormat;
	}

	if (HasSingleMip(Export)) {
		TextureCube->MipGenSettings = TMGS_NoMipmaps;
	}

	if (!BuildSourceFromRawMip(TextureCube, Data, Cooked)) {
		return false;
	}

	OutTextureCube = TextureCube;

	return true;
}

bool FTextureCreator::CreateVolumeTexture(UTexture*& OutVolumeTexture, TArray<uint8>& Data, const TSharedPtr<FJsonObject>& Export) {
	if (!RequireRawMipData(TEXT("A volume"))) {
		return false;
	}

	UVolumeTexture* VolumeTexture = NewObject<UVolumeTexture>(Package, UVolumeTexture::StaticClass(), *AssetName, RF_Public | RF_Standalone);

	DeserializeTexture(VolumeTexture, Export);
	SetPlatformData(VolumeTexture, new FTexturePlatformData());

	/* Depth varies per asset, so fall back to reading it off the width */
	FTextureCookedLayout Cooked;
	if (!GetCookedLayout(Export, 0, Cooked)) {
		return false;
	}

	Cooked.SlicesHalvePerMip = true;

	if (FTexturePlatformData* PlatformData = GetPlatformData(VolumeTexture)) {
		PlatformData->PixelFormat = Cooked.PixelFormat;
	}

	if (HasSingleMip(Export)) {
		VolumeTexture->MipGenSettings = TMGS_NoMipmaps;
	}

	if (!BuildSourceFromRawMip(VolumeTexture, Data, Cooked)) {
		return false;
	}

	OutVolumeTexture = VolumeTexture;

	return true;
}

bool FTextureCreator::CreateRenderTarget2D(UTexture*& OutRenderTarget2D, const TSharedPtr<FJsonObject>& Export) {
	UTextureRenderTargetFactoryNew* TextureFactory = NewObject<UTextureRenderTargetFactoryNew>();
	TextureFactory->AddToRoot();
	UTextureRenderTarget2D* RenderTarget2D = Cast<UTextureRenderTarget2D>(TextureFactory->FactoryCreateNew(UTextureRenderTarget2D::StaticClass(), Package, *AssetName, RF_Standalone | RF_Public, nullptr, GWarn));

	if (RenderTarget2D == nullptr) {
		return false;
	}

	DeserializeTexture(RenderTarget2D, Export);

	/* A render target is created empty, so its size is a property of its own rather than
	 * something cooked, and it never carries a payload to read one off */
	const TSharedPtr<FJsonObject> Properties = GetProperties(Export);

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

	bool bAutoGenerateMips = false;
	if (Properties->TryGetBoolField(TEXT("bAutoGenerateMips"), bAutoGenerateMips)) RenderTarget2D->bAutoGenerateMips = bAutoGenerateMips;
	/* Render targets only gained a mip sampler filter alongside auto generated mips in 4.23 */
#if !UE4_22_BELOW
	if (bAutoGenerateMips) {
		FString MipsSamplerFilter;

		if (Properties->TryGetStringField(TEXT("MipsSamplerFilter"), MipsSamplerFilter))
			RenderTarget2D->MipsSamplerFilter = static_cast<TextureFilter>(StaticEnum<TextureFilter>()->GetValueByNameString(MipsSamplerFilter));
	}
#endif

	OutRenderTarget2D = RenderTarget2D;

	return true;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Source data ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

int64 FTextureCookedLayout::GetEncodedSize() const {
	return Source.GetEncodedSliceSize(SizeX, SizeY) * SizeZ;
}

bool FTextureCookedLayout::DropMip() {
	/* Block compressed data bottoms out at a single block, and the decoders count whole blocks */
	const int32 Smallest = Source.BlockDim;

	if (SizeX <= Smallest && SizeY <= Smallest) {
		return false;
	}

	SizeX = FMath::Max(Smallest, SizeX / 2);
	SizeY = FMath::Max(Smallest, SizeY / 2);

	if (SlicesHalvePerMip) {
		SizeZ = FMath::Max(1, SizeZ / 2);
	}

	return true;
}

bool FTextureCreator::GetCookedLayout(const TSharedPtr<FJsonObject>& Export, const int32 FallbackSlices, FTextureCookedLayout& OutCooked) const {
	int32 SizeX = 0;
	int32 ReportedSizeY = 0;

	Export->TryGetNumberField(TEXT("SizeX"), SizeX);
	Export->TryGetNumberField(TEXT("SizeY"), ReportedSizeY);

	/* Two shapes of export reach here. Cloud used to fold every slice into the height and flatten
	 * the mip's depth to one, so the height had to be divided back down. It now leaves the height
	 * alone and writes the real depth on each mip, which is the same thing the archive holds.
	 *
	 * Mip zero tells them apart without having to know which Core produced the dump: it says how
	 * many bytes it holds, and only one of the two readings accounts for all of them. */
	int32 MipSlices = 0;
	int64 MipPayload = 0;
	GetFirstMipDescription(Export, MipSlices, MipPayload);

	int32 Slices = MipSlices > 1 ? MipSlices : 0;

	if (Slices <= 0 && (!Export->TryGetNumberField(TEXT("SizeZ"), Slices) || Slices <= 0)) {
		Slices = FallbackSlices > 0 ? FallbackSlices : GetReportedSliceCount(Export);

		if (Slices <= 0) {
			Slices = SizeX > 0 ? ReportedSizeY / SizeX : 0;

			UE_LOG(LogReflection, Warning, TEXT("\"%s\" was sent without a slice count, reading %d off its width. Update Core to import this reliably."), *AssetName, Slices);
		}
	}

	if (Slices <= 0) {
		UE_LOG(LogReflection, Error, TEXT("\"%s\" was cooked at an unusable size (%d x %d over %d slices)"), *AssetName, SizeX, ReportedSizeY, Slices);

		return false;
	}

	int32 SizeY = ReportedSizeY;

	/* The height carries every slice when it alone accounts for the whole payload */
	FTextureSourceLayout Layout;
	FString PixelFormatName;
	Export->TryGetStringField(TEXT("PixelFormat"), PixelFormatName);

	if (MipPayload > 0 && FTextureFormats::GetLayout(FTextureFormats::FromName(PixelFormatName), Layout)
		&& Layout.GetEncodedSliceSize(SizeX, ReportedSizeY) == MipPayload) {
		SizeY = ReportedSizeY / Slices;

		if (SizeY * Slices != ReportedSizeY) {
			UE_LOG(LogReflection, Error, TEXT("\"%s\" was cooked at an unusable size (%d x %d over %d slices)"), *AssetName, SizeX, ReportedSizeY, Slices);

			return false;
		}
	}

	return ReadCookedLayout(Export, SizeX, SizeY, Slices, OutCooked);
}

void FTextureCreator::GetFirstMipDescription(const TSharedPtr<FJsonObject>& Export, int32& OutSlices, int64& OutPayload) {
	OutSlices = 0;
	OutPayload = 0;

	const TArray<TSharedPtr<FJsonValue>>* Mips;
	if (!Export->TryGetArrayField(TEXT("Mips"), Mips) || Mips->Num() == 0) return;

	const TSharedPtr<FJsonObject> Mip = (*Mips)[0]->AsObject();
	if (!Mip.IsValid()) return;

	Mip->TryGetNumberField(TEXT("SizeZ"), OutSlices);

	const TSharedPtr<FJsonObject>* BulkData;
	if (Mip->TryGetObjectField(TEXT("BulkData"), BulkData)) {
		(*BulkData)->TryGetNumberField(TEXT("ElementCount"), OutPayload);
	}
}

bool FTextureCreator::GetCookedArrayLayout(const TSharedPtr<FJsonObject>& Export, FTextureCookedLayout& OutCooked) const {
	int32 SizeX = 0;
	int32 SizeY = 0;

	Export->TryGetNumberField(TEXT("SizeX"), SizeX);
	Export->TryGetNumberField(TEXT("SizeY"), SizeY);

	/* Nothing was folded into the height here, so the count is whatever the export says it is */
	const int32 Slices = GetReportedSliceCount(Export);

	return ReadCookedLayout(Export, SizeX, SizeY, Slices > 0 ? Slices : 1, OutCooked);
}

bool FTextureCreator::ReadCookedLayout(const TSharedPtr<FJsonObject>& Export, const int32 SizeX, const int32 SizeY, const int32 Slices, FTextureCookedLayout& OutCooked) const {
	OutCooked.SizeX = SizeX;
	OutCooked.SizeY = SizeY;
	OutCooked.SizeZ = Slices;

	FString PixelFormatName;
	Export->TryGetStringField(TEXT("PixelFormat"), PixelFormatName);
	OutCooked.PixelFormat = FTextureFormats::FromName(PixelFormatName);

	if (SizeX <= 0 || SizeY <= 0 || Slices <= 0) {
		UE_LOG(LogReflection, Error, TEXT("\"%s\" was cooked at an unusable size (%d x %d over %d slices)"), *AssetName, SizeX, SizeY, Slices);

		return false;
	}

	if (!FTextureFormats::GetLayout(OutCooked.PixelFormat, OutCooked.Source)) {
		UE_LOG(LogReflection, Error, TEXT("\"%s\" was cooked as %s, which has no source format mapping yet"), *AssetName, *PixelFormatName);

		return false;
	}

	return true;
}

int32 FTextureCreator::GetReportedSliceCount(const TSharedPtr<FJsonObject>& Export) {
	/* 4.25 packed the slice count in with the cube map and opt data bits. Before that it stood on
	 * its own, and the dump names it accordingly, so both spellings are read. */
	uint32 PackedData = 0;
	if (Export->TryGetNumberField(TEXT("PackedData"), PackedData)) {
		constexpr uint32 SliceMask = (1u << 30) - 1u;

		return static_cast<int32>(PackedData & SliceMask);
	}

	int32 NumSlices = 0;
	Export->TryGetNumberField(TEXT("NumSlices"), NumSlices);

	return NumSlices;
}

bool FTextureCreator::BuildSourceFromRawMip(UTexture* Texture, const TArray<uint8>& Data, const FTextureCookedLayout& Cooked) const {
	/* Cloud sends the first mip that still has its bulk data, which isn't always the top one:
	 * higher mips can live in a pak that isn't mounted. The payload size says which mip actually
	 * came through, so walk the chain until it fits rather than decoding at the wrong size. */
	FTextureCookedLayout Mip = Cooked;

	/* The size has to land exactly. Settling for "big enough" would happily decode the wrong mip
	 * at the wrong dimensions and hand back a plausible looking asset full of noise. */
	while (Data.Num() != Mip.GetEncodedSize()) {
		if (!Mip.DropMip()) {
			UE_LOG(LogReflection, Error, TEXT("\"%s\" came back with %d bytes, which matches no mip of a %d x %d x %d %s texture"), *AssetName, Data.Num(), Cooked.SizeX, Cooked.SizeY, Cooked.SizeZ, *UTexture::GetPixelFormatEnum()->GetNameStringByValue(Cooked.PixelFormat));

			return false;
		}
	}

	/* A texture that came out at a fraction of the size it was cooked at is a wrong asset rather
	 * than a note in passing: it imports, it looks right in a thumbnail, and it is soft everywhere
	 * it gets used. Said as loudly as anything that failed outright, and put on the report so it
	 * is still there once the log has scrolled. */
	if (Mip.SizeX != Cooked.SizeX) {
		UE_LOG(LogReflection, Error, TEXT("\"%s\" only had mip data down at %d x %d x %d, importing that instead of %d x %d x %d"), *AssetName, Mip.SizeX, Mip.SizeY, Mip.SizeZ, Cooked.SizeX, Cooked.SizeY, Cooked.SizeZ);

		FImportIssues::ReportFor(
			AssetName,
			Package != nullptr ? Package->GetName() : FString(),
			Texture != nullptr ? Texture->GetClass()->GetName() : FString(),
			EImportIssue::Data,
			FString::Printf(
				TEXT("Imported at %d x %d x %d instead of %d x %d x %d"),
				Mip.SizeX, Mip.SizeY, Mip.SizeZ, Cooked.SizeX, Cooked.SizeY, Cooked.SizeZ
			),
			TEXT("The mips above that one weren't in a mounted pak, so this is all there was to import.")
		);
	}

	const int64 EncodedSliceSize = Mip.Source.GetEncodedSliceSize(Mip.SizeX, Mip.SizeY);
	const int64 DecodedSliceSize = Mip.Source.GetDecodedSliceSize(Mip.SizeX, Mip.SizeY);

	uint8* SourceData = static_cast<uint8*>(FMemory::Malloc(DecodedSliceSize * Mip.SizeZ));

	for (int32 SliceIndex = 0; SliceIndex < Mip.SizeZ; SliceIndex++) {
		const uint8* EncodedSlice = Data.GetData() + EncodedSliceSize * SliceIndex;
		uint8* DecodedSlice = SourceData + DecodedSliceSize * SliceIndex;

		if (Mip.Source.RequiresDecode) {
			FTextureFormats::Decode(EncodedSlice, DecodedSlice, Mip.SizeX, Mip.SizeY, DecodedSliceSize, Mip.PixelFormat);
		} else {
			FMemory::Memcpy(DecodedSlice, EncodedSlice, DecodedSliceSize);
		}
	}

	Texture->Source.Init(Mip.SizeX, Mip.SizeY, Mip.SizeZ, 1, Mip.Source.SourceFormat, SourceData);

	FMemory::Free(SourceData);

	if (Texture->LODGroup == 255) {
		Texture->LODGroup = TEXTUREGROUP_World;
	}

	Texture->UpdateResource();

	/* An import keeps the editor drawn while it waits on the Cloud, so the renderer is drawing the
	 * whole time this runs, and updating a texture swaps the resource a material has already cached
	 * the handles of. Waited on here, so the swap is finished before the next thing drawn can read
	 * what a material points at. */
	FlushRenderingCommands();

	return true;
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Deserialization ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

void FTextureCreator::DeserializeTexture(UTexture* Texture, const TSharedPtr<FJsonObject>& Export) const {
	if (Texture == nullptr) return;

	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(GetProperties(Export), {
		"ImportedSize",
		"LODBias"
	}), Texture);
}

void FTextureCreator::DeserializeTexture2D(UTexture2D* Texture2D, const TSharedPtr<FJsonObject>& Export) const {
	if (Texture2D == nullptr) return;

	DeserializeTexture(Texture2D, Export);

	const TSharedPtr<FJsonObject> Properties = GetProperties(Export);

	FString AddressX;
	FString AddressY;
	bool bHasBeenPaintedInEditor;

	if (Properties->TryGetStringField(TEXT("AddressX"), AddressX)) Texture2D->AddressX = static_cast<TextureAddress>(StaticEnum<TextureAddress>()->GetValueByNameString(AddressX));
	if (Properties->TryGetStringField(TEXT("AddressY"), AddressY)) Texture2D->AddressY = static_cast<TextureAddress>(StaticEnum<TextureAddress>()->GetValueByNameString(AddressY));
	if (Properties->TryGetBoolField(TEXT("bHasBeenPaintedInEditor"), bHasBeenPaintedInEditor)) Texture2D->bHasBeenPaintedInEditor = bHasBeenPaintedInEditor;

	int FirstResourceMemMip;
	int LevelIndex;

	if (Properties->TryGetNumberField(TEXT("FirstResourceMemMip"), FirstResourceMemMip)) Texture2D->FirstResourceMemMip = FirstResourceMemMip;
	if (Properties->TryGetNumberField(TEXT("LevelIndex"), LevelIndex)) Texture2D->LevelIndex = LevelIndex;

	/* ~~~~~~~~~~~~~ Platform Data ~~~~~~~~~~~~~ */
	FTexturePlatformData* PlatformData = GetPlatformData(Texture2D);
	if (PlatformData == nullptr) return;

	/* Cooked sizes live on the export rather than under its properties */
	int SizeX;
	int SizeY;
#if !UE4_23_BELOW
	uint32 PackedData;
#endif
	FString PixelFormat;

	if (Export->TryGetNumberField(TEXT("SizeX"), SizeX)) PlatformData->SizeX = SizeX;
	if (Export->TryGetNumberField(TEXT("SizeY"), SizeY)) PlatformData->SizeY = SizeY;
	/* 4.24 packed the slice count together with the cube map and opt data bits into PackedData.
	 * Before that the slice count stood on its own, and the dump names it accordingly. */
#if UE4_23_BELOW
	int NumSlices;
	if (Export->TryGetNumberField(TEXT("NumSlices"), NumSlices)) PlatformData->NumSlices = NumSlices;
#else
	if (Export->TryGetNumberField(TEXT("PackedData"), PackedData)) PlatformData->PackedData = PackedData;
#endif
	if (Export->TryGetStringField(TEXT("PixelFormat"), PixelFormat)) PlatformData->PixelFormat = FTextureFormats::FromName(PixelFormat);
}

bool FTextureCreator::HasSingleMip(const TSharedPtr<FJsonObject>& Export) {
	const TArray<TSharedPtr<FJsonValue>>* Mips;

	return Export->TryGetArrayField(TEXT("Mips"), Mips) && Mips->Num() == 1;
}

TSharedPtr<FJsonObject> FTextureCreator::GetProperties(const TSharedPtr<FJsonObject>& Export) {
	const TSharedPtr<FJsonObject>* Properties = nullptr;

	if (Export.IsValid() && Export->TryGetObjectField(TEXT("Properties"), Properties)) {
		return *Properties;
	}

	return MakeShared<FJsonObject>();
}
