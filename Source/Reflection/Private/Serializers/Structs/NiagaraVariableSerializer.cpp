/* Copyright Reflection Contributors 2024-2026 */

#include "Serializers/Structs/NiagaraVariableSerializer.h"

#include "Engine/Compatibility.h"

#include "NiagaraTypes.h"

namespace {
	/* The type a variable was written down as.
	 *
	 * Named in the export by the struct, class or enum standing behind it, and said outright which
	 * of the three it is, so nothing here has to choose between a struct and a class sharing a
	 * name. Looked up by the module it came from where the export gives one, since a bare name
	 * matches whatever the search reaches first. */
	bool ReadType(const TSharedPtr<FJsonObject>& Entry, FNiagaraTypeDefinition& OutType) {
		const TSharedPtr<FJsonObject>* TypeDef;

		if (!Entry->TryGetObjectField(TEXT("TypeDef"), TypeDef)) return false;

		const TSharedPtr<FJsonObject>* Named;

		if (!(*TypeDef)->TryGetObjectField(TEXT("ClassStructOrEnum"), Named)) return false;

		FString ObjectName;

		if (!(*Named)->TryGetStringField(TEXT("ObjectName"), ObjectName)) return false;

		/* Written as Type'Name', and only the name is wanted */
		FString Leaf = ObjectName;

		if (Leaf.Contains(TEXT("'"))) {
			Leaf.Split(TEXT("'"), nullptr, &Leaf);

			Leaf = Leaf.Replace(TEXT("'"), TEXT(""));
		}

		if (Leaf.IsEmpty()) return false;

		FString Module;

		(*Named)->TryGetStringField(TEXT("ObjectPath"), Module);

		const FString Full = Module.IsEmpty() ? FString() : Module + TEXT(".") + Leaf;

		int32 Kind = 0;

		(*TypeDef)->TryGetNumberField(TEXT("UnderlyingType"), Kind);

		if (Kind == FNiagaraTypeDefinition::UT_Struct) {
			UScriptStruct* Struct = Full.IsEmpty() ? nullptr : FindObject<UScriptStruct>(nullptr, *Full);

			if (Struct == nullptr) Struct = FindStructByType(Leaf);
			if (Struct == nullptr) return false;

			/* Taken whatever it is. A struct Niagara would not normally accept still has the size
			 * the rest of the export was written against, and turning it down leaves the variable
			 * at the default, which is the thing being put right here. */
#if ENGINE_UE5
			OutType = FNiagaraTypeDefinition(Struct, FNiagaraTypeDefinition::EAllowUnfriendlyStruct::Allow);
#else
			OutType = FNiagaraTypeDefinition(Struct);
#endif
		} else if (Kind == FNiagaraTypeDefinition::UT_Enum) {
			UEnum* Enum = Full.IsEmpty() ? nullptr : FindObject<UEnum>(nullptr, *Full);

			if (Enum == nullptr) Enum = FindEnumByType(Leaf);
			if (Enum == nullptr) return false;

			OutType = FNiagaraTypeDefinition(Enum);
		} else if (Kind == FNiagaraTypeDefinition::UT_Class) {
			UClass* Class = Full.IsEmpty() ? nullptr : FindObject<UClass>(nullptr, *Full);

			if (Class == nullptr) Class = FindClassByType(Leaf);
			if (Class == nullptr) return false;

			OutType = FNiagaraTypeDefinition(Class);
		} else return false;

#if ENGINE_UE5
		/* Whether the graph reads this one while it compiles rather than while it runs, which the
		 * type carries and the value does not */
		if (int32 Flags = 0; (*TypeDef)->TryGetNumberField(TEXT("Flags"), Flags) && Flags != 0) {
			OutType.SetFlags(static_cast<FNiagaraTypeDefinition::FTypeFlags>(Flags));
		}
#endif

		return true;
	}
}

void FNiagaraVariableSerializer::Deserialize(UScriptStruct* Struct, void* StructData, const TSharedPtr<FJsonObject> JsonValue, UObject* OptionalOuter) {
	Fallback.Deserialize(Struct, StructData, JsonValue, OptionalOuter);

	if (Struct == nullptr || StructData == nullptr || !JsonValue.IsValid()) return;

	if (FNiagaraTypeDefinition Type; ReadType(JsonValue, Type)) {
		static_cast<FNiagaraVariableBase*>(StructData)->SetType(Type);
	}

	/* A value the size of the type it is now. One written at another size is a value nothing can
	 * read, and a variable that arrived with none had none to write. */
	if (Struct->IsChildOf(FNiagaraVariable::StaticStruct())) {
		static_cast<FNiagaraVariable*>(StructData)->AllocateData();
	}
}
