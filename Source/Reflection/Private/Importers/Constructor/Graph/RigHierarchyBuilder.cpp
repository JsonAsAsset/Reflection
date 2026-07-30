/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/RigHierarchyBuilder.h"

#include "Modules/Log.h"
#include "Serializers/PropertySerializer.h"

#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Rigs/RigHierarchyElements.h"

FRigHierarchyBuilder::FRigHierarchyBuilder(UBlueprint* InBlueprint, FUObjectExportContainer* InContainer, UPropertySerializer* InPropertySerializer)
	: Blueprint(InBlueprint)
	, Container(InContainer)
	, PropertySerializer(InPropertySerializer)
{
	/* Assets that own a hierarchy say so through an interface, which saves guessing at which property holds it */
	if (IRigHierarchyProvider* Provider = Cast<IRigHierarchyProvider>(InBlueprint)) {
		Hierarchy = Provider->GetHierarchy();
	}
}

bool FRigHierarchyBuilder::ReadStruct(const TSharedPtr<FJsonObject>& Json, UScriptStruct* Struct, void* OutValue) const {
	if (!Json.IsValid() || Struct == nullptr || PropertySerializer == nullptr) return false;

	PropertySerializer->DeserializeStruct(Struct, Json.ToSharedRef(), OutValue);

	return true;
}

FRigElementKey FRigHierarchyBuilder::ReadParentKey(const TSharedPtr<FJsonObject>& Element) const {
	FRigElementKey ParentKey;

	/* Bones and sockets have exactly one parent and name it outright */
	const TSharedPtr<FJsonObject>* Parent;
	if (Element->TryGetObjectField(TEXT("ParentKey"), Parent)) {
		ReadStruct(*Parent, FRigElementKey::StaticStruct(), &ParentKey);

		return ParentKey;
	}

	/* Nulls and controls can have several, weighted; the first is the one to parent under */
	const TArray<TSharedPtr<FJsonValue>>* Constraints;
	if (!Element->TryGetArrayField(TEXT("ParentConstraints"), Constraints) || Constraints->Num() == 0) {
		return ParentKey;
	}

	const TSharedPtr<FJsonObject>* ParentElement;
	if (!(*Constraints)[0]->AsObject()->TryGetObjectField(TEXT("ParentElement"), ParentElement)) {
		return ParentKey;
	}

	const TSharedPtr<FJsonObject>* ParentElementKey;
	if ((*ParentElement)->TryGetObjectField(TEXT("LoadedKey"), ParentElementKey)) {
		ReadStruct(*ParentElementKey, FRigElementKey::StaticStruct(), &ParentKey);
	}

	return ParentKey;
}

FTransform FRigHierarchyBuilder::ReadLocalTransform(const TSharedPtr<FJsonObject>& Element, const FString& StorageName) const {
	FTransform Transform = FTransform::Identity;

	const TSharedPtr<FJsonObject>* Storage;
	if (!Element->TryGetObjectField(StorageName, Storage)) return Transform;

	/* The initial pose is the rig as it was authored; current is wherever it happened to be left */
	const TSharedPtr<FJsonObject>* Pose;
	if (!(*Storage)->TryGetObjectField(TEXT("Initial"), Pose) && !(*Storage)->TryGetObjectField(TEXT("Current"), Pose)) {
		return Transform;
	}

	const TSharedPtr<FJsonObject>* Local;
	if (!(*Pose)->TryGetObjectField(TEXT("Local"), Local)) return Transform;

	const TSharedPtr<FJsonObject>* Value;
	if (!(*Local)->TryGetObjectField(TEXT("Transform"), Value)) return Transform;

	ReadStruct(*Value, TBaseStructure<FTransform>::Get(), &Transform);

	return Transform;
}

bool FRigHierarchyBuilder::AddElement(const FRigElementKey& Key, const TSharedPtr<FJsonObject>& Element) {
	const FRigElementKey Parent = ReadParentKey(Element);
	const FTransform Transform = ReadLocalTransform(Element, TEXT("PoseStorage"));

	/* The transforms read out of the pose are local, so the controller shouldn't take them for world space */
	constexpr bool bTransformInGlobal = false;
	constexpr bool bSetupUndo = false;

	switch (Key.Type) {
		case ERigElementType::Bone: {
			int32 ExportedBoneType = static_cast<int32>(ERigBoneType::User);
			Element->TryGetNumberField(TEXT("BoneType"), ExportedBoneType);

			return Controller->AddBone(Key.Name, Parent, Transform, bTransformInGlobal, static_cast<ERigBoneType>(ExportedBoneType), bSetupUndo).IsValid();
		}

		case ERigElementType::Null:
			return Controller->AddNull(Key.Name, Parent, Transform, bTransformInGlobal, bSetupUndo).IsValid();

		case ERigElementType::Control: {
			FRigControlSettings Settings;
			FRigControlValue Value;

			const TSharedPtr<FJsonObject>* ExportedSettings;
			if (Element->TryGetObjectField(TEXT("Settings"), ExportedSettings)) {
				ReadStruct(*ExportedSettings, FRigControlSettings::StaticStruct(), &Settings);
			}

			return Controller->AddControl(
				Key.Name,
				Parent,
				Settings,
				Value,
				ReadLocalTransform(Element, TEXT("Offset")),
				ReadLocalTransform(Element, TEXT("Shape")),
				bSetupUndo
			).IsValid();
		}

		case ERigElementType::Curve: {
			double ExportedValue = 0.0;
			Element->TryGetNumberField(TEXT("Value"), ExportedValue);

			return Controller->AddCurve(Key.Name, static_cast<float>(ExportedValue), bSetupUndo).IsValid();
		}

		case ERigElementType::Socket:
			return Controller->AddSocket(Key.Name, Parent, Transform, bTransformInGlobal, FLinearColor::White, FString(), bSetupUndo).IsValid();

		case ERigElementType::Connector: {
			FRigConnectorSettings Settings;

			const TSharedPtr<FJsonObject>* ExportedSettings;
			if (Element->TryGetObjectField(TEXT("Settings"), ExportedSettings)) {
				ReadStruct(*ExportedSettings, FRigConnectorSettings::StaticStruct(), &Settings);
			}

			return Controller->AddConnector(Key.Name, Settings, bSetupUndo).IsValid();
		}

		default:
			return false;
	}
}

bool FRigHierarchyBuilder::Build() {
	if (Hierarchy == nullptr || Container == nullptr) return false;

	/* The hierarchy export is found by carrying elements rather than by its type name, so this doesn't care what
	 * the host asset calls it or which engine version cooked it */
	const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;

	for (FUObjectExport* Export : Container->Exports) {
		const TArray<TSharedPtr<FJsonValue>>* Candidate;

		if (!Export->JsonObject.IsValid() || !Export->JsonObject->TryGetArrayField(TEXT("Elements"), Candidate)) continue;
		if (Candidate->Num() == 0 || !(*Candidate)[0]->AsObject()->HasField(TEXT("LoadedKey"))) continue;

		Elements = Candidate;
		break;
	}

	if (Elements == nullptr) return false;

	Controller = Hierarchy->GetController(true);
	if (Controller == nullptr) return false;

	/* Whatever the blueprint was created with is a starting point, not something to append to */
	Hierarchy->Reset();

	int32 Added = 0;

	/* Elements are stored in the order the rig was built up, which is not the order they can be created in - a
	 * chain of bones can be authored before the master it hangs off, and naming a parent that doesn't exist yet
	 * silently lands the element at the root. So each one pulls its parent in ahead of itself. */
	TMap<FName, int32> ElementIndices;
	TArray<FRigElementKey> Keys;

	Keys.Reserve(Elements->Num());

	for (const TSharedPtr<FJsonValue>& Value : *Elements) {
		FRigElementKey Key;

		const TSharedPtr<FJsonObject> Element = Value->AsObject();
		const TSharedPtr<FJsonObject>* ExportedKey;

		if (Element.IsValid() && Element->TryGetObjectField(TEXT("LoadedKey"), ExportedKey)) {
			ReadStruct(*ExportedKey, FRigElementKey::StaticStruct(), &Key);
		}

		if (Key.IsValid()) {
			ElementIndices.Add(Key.Name, Keys.Num());
		}

		Keys.Add(Key);
	}

	TArray<bool> bVisited;
	bVisited.Init(false, Keys.Num());

	TFunction<void(int32)> AddWithParent = [&](const int32 Index) {
		/* Marked before recursing, so a hierarchy that somehow loops back on itself still terminates */
		if (bVisited[Index]) return;
		bVisited[Index] = true;

		if (!Keys[Index].IsValid()) return;

		const TSharedPtr<FJsonObject> Element = (*Elements)[Index]->AsObject();
		const FRigElementKey Parent = ReadParentKey(Element);

		if (const int32* ParentIndex = ElementIndices.Find(Parent.Name)) {
			AddWithParent(*ParentIndex);
		}

		if (AddElement(Keys[Index], Element)) {
			Added++;
		} else {
			UE_LOG(LogReflection, Warning, TEXT("Could not add rig element \"%s\""), *Keys[Index].Name.ToString());
		}
	};

	for (int32 Index = 0; Index < Keys.Num(); Index++) {
		AddWithParent(Index);
	}

	UE_LOG(LogReflection, Log, TEXT("Rebuilt %d of %d rig hierarchy elements"), Added, Elements->Num());

	return Added > 0;
}
