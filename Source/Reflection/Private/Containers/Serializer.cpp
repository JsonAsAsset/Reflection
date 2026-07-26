/* Copyright Reflection Contributors 2024-2026 */

#include "Containers/Serializer.h"

USerializerContainer::USerializerContainer() {
	CreateSerializer();
}

void USerializerContainer::Initialize(FUObjectExport* Export, FUObjectExportContainer* Container) {
	AssetContainer = Container;
	AssetExport = Export;
	
	/* Create Properties field if it doesn't exist */
	if (!AssetExport->JsonObject->HasField(TEXT("Properties"))) {
		AssetExport->JsonObject->SetObjectField(TEXT("Properties"), TSharedPtr<FJsonObject>());
	}

	/* Move asset properties defined outside "Properties" and move it inside */
	for (const auto& Pair : AssetExport->JsonObject->Values) {
		const FString& PropertyName = Pair.Key;
    
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
