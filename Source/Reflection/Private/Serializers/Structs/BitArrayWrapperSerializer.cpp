/* Copyright Reflection Contributors 2024-2026 */

#include "Serializers/Structs/BitArrayWrapperSerializer.h"

#include "Dom/JsonObject.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/Class.h"

void FBitArrayWrapperSerializer::Deserialize(UScriptStruct* Struct, void* StructData, const TSharedPtr<FJsonObject> JsonValue, UObject* OptionalOuter) {
	/* The wrapper holds a bit array, which carries no reflection data, so like the connectivity cube it has
	 * nothing for the fallback serializer to write through. The bytes its own serializer reads are built here
	 * and fed back to it instead. */
	TArray<uint8> WrapperBytes;
	FMemoryWriter Writer(WrapperBytes);

	const TArray<TSharedPtr<FJsonValue>>* Bits = nullptr;

	int32 NumBits = JsonValue->TryGetArrayField(TEXT("BitArray"), Bits) ? Bits->Num() : 0;

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

	FMemoryReader Reader(WrapperBytes);
	Struct->SerializeItem(Reader, StructData, nullptr);
}
