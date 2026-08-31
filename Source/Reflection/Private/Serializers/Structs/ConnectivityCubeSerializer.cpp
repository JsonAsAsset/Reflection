/* Copyright Reflection Contributors 2024-2026 */

#include "Serializers/Structs/ConnectivityCubeSerializer.h"

#include "Dom/JsonObject.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/Class.h"

/* The faces in the order the cube writes them, which is the order of EFortConnectivityCubeFace. Named rather
 * than positional, so a json object that lists them in some other order still lands each face in its place. */
static const TCHAR* ConnectivityCubeFaceNames[] = {
	TEXT("Front"),
	TEXT("Left"),
	TEXT("Back"),
	TEXT("Right"),
	TEXT("Upper"),
	TEXT("Lower")
};

void FConnectivityCubeSerializer::Deserialize(UScriptStruct* Struct, void* StructData, const TSharedPtr<FJsonObject> JsonValue, UObject* OptionalOuter) {
	/* Each face is a bit array, which carries no reflection data, so the cube has no properties to write
	 * through and the fallback serializer finds nothing to do. What it does have is a serializer of its own,
	 * so the bytes that serializer would have read are built here and fed back to it. That leaves the layout
	 * of the struct its own business, which is what keeps this out of the game module. */
	TArray<uint8> CubeBytes;
	FMemoryWriter Writer(CubeBytes);

	for (const TCHAR* FaceName : ConnectivityCubeFaceNames) {
		const TArray<TSharedPtr<FJsonValue>>* Bits = nullptr;

		/* A face the export left out is written as an empty one rather than skipped, since the cube reads all
		 * six back in a fixed order and a missing face would put every face after it out of step. */
		int32 NumBits = JsonValue->TryGetArrayField(FaceName, Bits) ? Bits->Num() : 0;

		Writer << NumBits;

		/* A bit array is stored as whole words, least significant bit first, and is written the same way. */
		const int32 NumWords = FMath::DivideAndRoundUp(NumBits, 32);

		for (int32 WordIndex = 0; WordIndex < NumWords; ++WordIndex) {
			uint32 Word = 0;

			for (int32 WordBitIndex = 0; WordBitIndex < 32; ++WordBitIndex) {
				const int32 BitIndex = WordIndex * 32 + WordBitIndex;

				if (BitIndex < NumBits && (*Bits)[BitIndex]->AsBool()) {
					Word |= 1u << WordBitIndex;
				}
			}

			Writer << Word;
		}
	}

	FMemoryReader Reader(CubeBytes);
	Struct->SerializeItem(Reader, StructData, nullptr);
}
