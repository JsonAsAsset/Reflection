/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/ItemDefinitionData.h"

#include "Serializers/ObjectSerializer.h"

namespace {
	/* What an entry carries, against the property it stands for.
	 *
	 * GeneratedTagsIndexes is left out on purpose: it is the cook's own index into a table this
	 * project hasn't got, and nothing here can turn it back into anything. */
	const TMap<FString, FString> Stands = {
		{ TEXT("Icon"),      TEXT("SmallPreviewImage") },
		{ TEXT("LargeIcon"), TEXT("LargePreviewImage") },
		{ TEXT("Tags"),      TEXT("GameplayTags")      },
		{ TEXT("Traits"),    TEXT("Traits")            },
		{ TEXT("Series"),    TEXT("Series")            },
		{ TEXT("Rarity"),    TEXT("Rarity")            },
		{ TEXT("MinLevel"),  TEXT("MinLevel")          },
		{ TEXT("MaxLevel"),  TEXT("MaxLevel")          },
	};
}

int32 FItemDefinitionData::Apply(UObject* Asset, const TSharedPtr<FJsonObject>& Data, const UObjectSerializer* Serializer) {
	if (Asset == nullptr || !Data.IsValid() || Serializer == nullptr) return 0;

	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;

	if (!Data->TryGetArrayField(TEXT("DataList"), List)) return 0;

	/* Gathered into one object and handed over together, so every value goes through the same
	 * reader the properties written outright go through */
	const TSharedPtr<FJsonObject> Said = MakeShared<FJsonObject>();

	int32 Wrote = 0;

	for (const TSharedPtr<FJsonValue>& Value : *List) {
		if (!Value.IsValid() || Value->Type != EJson::Object) continue;

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Carried : Value->AsObject()->Values) {
			const FString* Called = Stands.Find(Carried.Key);

			if (Called == nullptr) continue;

			/* Said outright as well, which is the older shape and the one that wins */
			if (Data->HasField(*Called)) continue;

			/* Nowhere to put it in this build, which is true of MinLevel and the traits here */
			if (FindFProperty<FProperty>(Asset->GetClass(), **Called) == nullptr) continue;

			Said->SetField(*Called, Carried.Value);

			Wrote++;
		}
	}

	if (Wrote > 0) {
		Serializer->DeserializeObjectProperties(Said, Asset);
	}

	return Wrote;
}
