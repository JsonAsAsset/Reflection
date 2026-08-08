/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BlueprintVariables.h"

#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"

#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace {
	/* The referenced object of a property field, ex: the Struct of a StructProperty.
	 * Looked up by name because the path a cooked export carries rarely exists in the project. */
	template <typename T>
	T* ResolveField(const TSharedPtr<FJsonObject>& Property, const TCHAR* FieldName) {
		const TSharedPtr<FJsonObject>* Reference;
		if (!Property->TryGetObjectField(FieldName, Reference)) {
			return nullptr;
		}

		FString ObjectName;
		if (!(*Reference)->TryGetStringField(TEXT("ObjectName"), ObjectName)) {
			return nullptr;
		}

		const FName Name = GetExportNameOfSubobject(ObjectName);
		if (Name.IsNone()) {
			return nullptr;
		}

		/* The path first, so a user defined struct or enum sitting in the project wins */
		FString ObjectPath;
		if ((*Reference)->TryGetStringField(TEXT("ObjectPath"), ObjectPath) && !ObjectPath.Contains(TEXT("/Script/"))) {
			int32 Dot;
			if (ObjectPath.FindLastChar(TEXT('.'), Dot)) {
				LeftInline(ObjectPath, Dot);
			}

			if (T* Loaded = LoadObjectByPath<T>(ObjectPath + TEXT(".") + Name.ToString())) {
				return Loaded;
			}
		}

#if UE5_1_BEYOND
		return FindFirstObject<T>(*Name.ToString());
#else
		return FindObject<T>(ANY_PACKAGE, *Name.ToString());
#endif
	}

	/* Numeric and text types, everything that needs no object to point at */
	bool GetSimplePinCategory(const FString& Type, FName& OutCategory, FName& OutSubCategory) {
		OutSubCategory = NAME_None;

		if (Type == TEXT("BoolProperty")) { OutCategory = UEdGraphSchema_K2::PC_Boolean; return true; }
		if (Type == TEXT("NameProperty")) { OutCategory = UEdGraphSchema_K2::PC_Name; return true; }
		if (Type == TEXT("StrProperty")) { OutCategory = UEdGraphSchema_K2::PC_String; return true; }
		if (Type == TEXT("TextProperty")) { OutCategory = UEdGraphSchema_K2::PC_Text; return true; }
		if (Type == TEXT("Int64Property")) { OutCategory = UEdGraphSchema_K2::PC_Int64; return true; }

		/* Blueprints only offer one signed integer and one byte below 64 bits */
		if (Type == TEXT("IntProperty") || Type == TEXT("Int16Property") || Type == TEXT("Int8Property")
		 || Type == TEXT("UInt16Property") || Type == TEXT("UInt32Property")) {
			OutCategory = UEdGraphSchema_K2::PC_Int;

			return true;
		}

		if (Type == TEXT("FloatProperty") || Type == TEXT("DoubleProperty")) {
	/* 5.0 split the old float pin into a real pin carrying its width as a sub category */
#if ENGINE_UE5
			OutCategory = UEdGraphSchema_K2::PC_Real;
			OutSubCategory = Type == TEXT("DoubleProperty") ? UEdGraphSchema_K2::PC_Double : UEdGraphSchema_K2::PC_Float;
#else
			OutCategory = UEdGraphSchema_K2::PC_Float;
#endif

			return true;
		}

		return false;
	}
}

bool FBlueprintVariables::IsUserVariable(const TSharedPtr<FJsonObject>& Property) {
	if (!Property.IsValid()) {
		return false;
	}

	FString Flags;
	Property->TryGetStringField(TEXT("PropertyFlags"), Flags);

	/* Anim graph node state, the ubergraph frame and the extension subsystems all carry no flags
	 * at all. What a user put on the blueprint is editable, visible to blueprints, or both. */
	return Flags.Contains(TEXT("Edit")) || Flags.Contains(TEXT("BlueprintVisible"));
}

bool FBlueprintVariables::GetPinType(const TSharedPtr<FJsonObject>& Property, FEdGraphPinType& OutPinType) {
	if (!Property.IsValid()) {
		return false;
	}

	FString Type;
	if (!Property->TryGetStringField(TEXT("Type"), Type)) {
		return false;
	}

	/* Containers describe themselves through the property they hold, so the element decides the
	 * type and the container only decides the shape */
	const TSharedPtr<FJsonObject>* Element = nullptr;

	if (Type == TEXT("ArrayProperty") && Property->TryGetObjectField(TEXT("Inner"), Element)) {
		if (!GetPinType(*Element, OutPinType)) return false;

		OutPinType.ContainerType = EPinContainerType::Array;

		return true;
	}

	if (Type == TEXT("SetProperty") && Property->TryGetObjectField(TEXT("ElementProp"), Element)) {
		if (!GetPinType(*Element, OutPinType)) return false;

		OutPinType.ContainerType = EPinContainerType::Set;

		return true;
	}

	if (Type == TEXT("MapProperty")) {
		const TSharedPtr<FJsonObject>* ValueProperty;

		if (!Property->TryGetObjectField(TEXT("KeyProp"), Element)) return false;
		if (!Property->TryGetObjectField(TEXT("ValueProp"), ValueProperty)) return false;

		FEdGraphPinType ValuePinType;

		if (!GetPinType(*Element, OutPinType)) return false;
		if (!GetPinType(*ValueProperty, ValuePinType)) return false;

		OutPinType.ContainerType = EPinContainerType::Map;
		OutPinType.PinValueType = FEdGraphTerminalType::FromPinType(ValuePinType);

		return true;
	}

	if (GetSimplePinCategory(Type, OutPinType.PinCategory, OutPinType.PinSubCategory)) {
		return true;
	}

	if (Type == TEXT("StructProperty")) {
		UScriptStruct* Struct = ResolveField<UScriptStruct>(Property, TEXT("Struct"));
		if (!Struct) return false;

		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = Struct;

		return true;
	}

	if (Type == TEXT("EnumProperty") || Type == TEXT("ByteProperty")) {
		/* A byte with no enum behind it is just a byte */
		UEnum* Enum = ResolveField<UEnum>(Property, TEXT("Enum"));

		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		OutPinType.PinSubCategoryObject = Enum;

		return Enum != nullptr || Type == TEXT("ByteProperty");
	}

	if (Type == TEXT("ObjectProperty") || Type == TEXT("SoftObjectProperty")) {
		UClass* Class = ResolveField<UClass>(Property, TEXT("PropertyClass"));
		if (!Class) return false;

		OutPinType.PinCategory = Type == TEXT("ObjectProperty") ? UEdGraphSchema_K2::PC_Object : UEdGraphSchema_K2::PC_SoftObject;
		OutPinType.PinSubCategoryObject = Class;

		return true;
	}

	if (Type == TEXT("ClassProperty") || Type == TEXT("SoftClassProperty")) {
		UClass* Class = ResolveField<UClass>(Property, TEXT("MetaClass"));
		if (!Class) return false;

		OutPinType.PinCategory = Type == TEXT("ClassProperty") ? UEdGraphSchema_K2::PC_Class : UEdGraphSchema_K2::PC_SoftClass;
		OutPinType.PinSubCategoryObject = Class;

		return true;
	}

	if (Type == TEXT("InterfaceProperty")) {
		UClass* Class = ResolveField<UClass>(Property, TEXT("InterfaceClass"));
		if (!Class) return false;

		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Interface;
		OutPinType.PinSubCategoryObject = Class;

		return true;
	}

	return false;
}

int32 FBlueprintVariables::Construct(UBlueprint* Blueprint, const TArray<TSharedPtr<FJsonValue>>& ChildProperties) {
	if (Blueprint == nullptr) {
		return 0;
	}

	int32 Added = 0;
	int32 Unsupported = 0;

	for (const TSharedPtr<FJsonValue>& ChildProperty : ChildProperties) {
		const TSharedPtr<FJsonObject> Property = ChildProperty->AsObject();

		if (!IsUserVariable(Property)) {
			continue;
		}

		FString Name;
		if (!Property->TryGetStringField(TEXT("Name"), Name) || Name.IsEmpty()) {
			continue;
		}

		const FName VariableName(*Name);

		/* Inherited from the parent, or already added on a previous run */
		if (FindFProperty<FProperty>(Blueprint->GeneratedClass, VariableName) != nullptr) {
			continue;
		}

		if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName) != INDEX_NONE) {
			continue;
		}

		FEdGraphPinType PinType;
		if (!GetPinType(Property, PinType)) {
			FString Type;
			Property->TryGetStringField(TEXT("Type"), Type);

			UE_LOG(LogReflection, Warning, TEXT("\"%s\" has no blueprint pin type for variable \"%s\" of type %s"), *Blueprint->GetName(), *Name, *Type);

			Unsupported++;

			continue;
		}

		if (FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, PinType)) {
			Added++;
		} else {
			UE_LOG(LogReflection, Warning, TEXT("\"%s\" would not take variable \"%s\""), *Blueprint->GetName(), *Name);
		}
	}

	if (Unsupported > 0) {
		UE_LOG(LogReflection, Warning, TEXT("\"%s\" added %d variables, %d had no usable type"), *Blueprint->GetName(), Added, Unsupported);
	}

	return Added;
}
