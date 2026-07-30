/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "UObject/Object.h"
#include "Containers/Export.h"
#include "Dom/JsonValue.h"
#include "Containers/ExportContainer.h"
#include "ObjectSerializer.generated.h"

class UPropertySerializer;

UCLASS()
class REFLECTION_API UObjectSerializer : public UObject {
    GENERATED_BODY()
public:
    UObjectSerializer();

    void SetPropertySerializer(UPropertySerializer* NewPropertySerializer);
    void SetupExports(const TArray<TSharedPtr<FJsonValue>>& InObjects);

    FORCEINLINE UPropertySerializer* GetPropertySerializer() const { return PropertySerializer; }

    void DeserializeObjectProperties(const TSharedPtr<FJsonObject>& Properties, UObject* Object) const;

    void SetExportForDeserialization(const TSharedPtr<FJsonObject>& JsonObject, UObject* Object);
    void DeserializeExports(FUObjectExportContainer* Container, bool CreateObjects = true);
    void DeserializeExport(FUObjectExport* Export, TMap<TSharedPtr<FJsonObject>, UObject*>& ExportsMap);

    /* New Generation */
    UObject* SpawnExport(FUObjectExport* Export, bool bOnlySerialize = false);

    /* Spawns exports as they are referenced */
    bool bUseExperimentalSpawning = false;

public:
    UPROPERTY()
    UObject* Parent;

    UPROPERTY()
    TMap<FString, UObject*> ConstructedObjects;
    
    UPROPERTY()
    UPropertySerializer* PropertySerializer;

    TArray<TSharedPtr<FJsonValue>> Exports;

    UPROPERTY()
    TArray<FString> ExportsToNotDeserialize;

    UPROPERTY()
    TArray<FString> WhitelistedTypes;
};