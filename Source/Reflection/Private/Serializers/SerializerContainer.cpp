/* Copyright Reflection Contributors 2024-2026 */

#include "Serializers/SerializerContainer.h"

USerializerContainer::USerializerContainer() {
	CreateSerializer();
}

void USerializerContainer::Initialize(FUObjectExport* Export, FUObjectExportContainer* Container) {
	AssetContainer = Container;
	AssetExport = Export;
	
	/* Create Properties field if it doesn't exist.
	 *
	 * Made as a real object rather than an empty handle. Setting an object field to a pointer
	 * holding nothing writes a null into the json, and asking for a null back as an object hands
	 * out the one empty object the json library keeps for saying no. Everything moved in below is
	 * then written into that, and it belongs to the process rather than to this export: every
	 * export after it that has no properties of its own reads another asset's, and what a class
	 * says it comes from is whatever the last one left in there. Which is why importing one asset
	 * is fine and importing one that drags in twenty is not. */
	if (!AssetExport->JsonObject->HasTypedField<EJson::Object>(TEXT("Properties"))) {
		AssetExport->JsonObject->SetObjectField(TEXT("Properties"), MakeShared<FJsonObject>());
	}

	/* Move asset properties defined outside "Properties" and move it inside */
	for (const auto& Pair : AssetExport->JsonObject->Values) {
		const FString PropertyName = JsonKeyToString(Pair.Key);
    
		if (!PropertyName.Equals(TEXT("Type")) &&
			!PropertyName.Equals(TEXT("Name")) &&
			!PropertyName.Equals(TEXT("Class")) &&
			!PropertyName.Equals(TEXT("Flags")) &&
			!PropertyName.Equals(TEXT("Properties"))
		) {
			AssetExport->GetProperties()->SetField(PropertyName, Pair.Value);
		}
	}

	AssetExport->NameOverride = AssetExport->GetName();
	
	/* BlueprintGeneratedClass is post-fixed with _C */
	if (AssetExport->GetType().ToString().Contains("BlueprintGeneratedClass")) {
		FString NewName; {
			AssetExport->NameOverride.ToString().Split("_C", &NewName, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			AssetExport->NameOverride = FName(*NewName);
		}
	}
	
	GetPropertySerializer()->ExportsContainer = AssetContainer;
}

UObjectSerializer* USerializerContainer::GetObjectSerializer() const {
	return ObjectSerializer;
}

UPropertySerializer* USerializerContainer::GetPropertySerializer() const {
	return GetObjectSerializer()->PropertySerializer;
}

void USerializerContainer::DeserializeExports(UObject* Parent, const bool CreateObjects) {
	GetObjectSerializer()->SetExportForDeserialization(GetAssetExport(), Parent);
	GetObjectSerializer()->Parent = Parent;
    
	GetObjectSerializer()->DeserializeExports(GetContainer(), CreateObjects);
	ApplyModifications();
}

void USerializerContainer::AddReferencedObjects(FReferenceCollector& Collector) {
	Collector.AddReferencedObject(ObjectSerializer);
}

void USerializerContainer::CreateSerializer() {
	ObjectSerializer = NewObject<UObjectSerializer>();
	GetObjectSerializer()->SetPropertySerializer(NewObject<UPropertySerializer>());
}

/* AssetExport ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~> */
FString USerializerContainer::GetAssetName() const {
	return AssetExport->GetName().ToString();
}

FString USerializerContainer::GetAssetType() const {
	return AssetExport->GetType().ToString();
}

TSharedPtr<FJsonObject> USerializerContainer::GetAssetData() const {
	return AssetExport->GetProperties();
}

FUObjectJsonValueExport USerializerContainer::GetAssetDataAsValue() const {
	return AssetExport->GetPropertiesAsValue();
}

FUObjectJsonValueExport USerializerContainer::GetAssetAsValue() const {
	return AssetExport->AsValueExport();
}

TSharedPtr<FJsonObject>& USerializerContainer::GetAssetExport() {
	return AssetExport->JsonObject;
}

UClass* USerializerContainer::GetAssetClass() {
	return AssetExport->GetClass();
}

void USerializerContainer::SetParent(UObject* Parent) {
	AssetExport->Parent = Parent;
}

UObject* USerializerContainer::GetAsset() {
	return AssetExport->Object;
}

void USerializerContainer::SetAsset(UObject* InAsset) {
	AssetExport->Object = InAsset;
}

FUObjectExportContainer* USerializerContainer::GetContainer() const {
	return AssetContainer;
}

UObject* USerializerContainer::GetParent() const {
	return AssetExport->Parent;
}

UPackage* USerializerContainer::GetPackage() const {
	return AssetExport->Package;
}

void USerializerContainer::SetPackage(UPackage* NewPackage) {
	AssetExport->Package = NewPackage;
}
/* AssetExport <~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
