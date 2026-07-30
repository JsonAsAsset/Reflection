/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

/* Reads the same way as FString::IsEmpty and TArray::IsEmpty at a call site, which TMap has no
 * member for on every engine version the plugin builds against */
template<typename K, typename V>
FORCEINLINE bool IsEmpty(const TMap<K, V>& Map) {
	return Map.Num() == 0;
}
