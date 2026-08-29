/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraParameterCollectionImporter.h"

#include "Engine/EngineUtilities.h"

#include "NiagaraParameterCollection.h"

namespace {
	/* The type a parameter was written down as.
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
			 * the offsets were written for, and turning it down leaves the parameter at the
			 * default, which is the thing being put right here. */
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

void INiagaraParameterCollectionImporter::Repair(UObject* Asset) const {
	UNiagaraParameterCollection* Collection = Cast<UNiagaraParameterCollection>(Asset);

	if (Collection == nullptr) return;

	const TArray<TSharedPtr<FJsonValue>>* Written;

	if (!GetAssetData()->TryGetArrayField(TEXT("Parameters"), Written)) return;

	/* What each parameter is, taken off the export. Kept by name because the store beside it lists
	 * the same parameters in an order of its own. */
	TMap<FName, FNiagaraTypeDefinition> Types;

	for (const TSharedPtr<FJsonValue>& Value : *Written) {
		const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;

		FString Named;

		if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("Name"), Named)) continue;

		if (FNiagaraTypeDefinition Type; ReadType(Entry, Type)) {
			Types.Add(FName(*Named), Type);
		}
	}

	if (Types.Num() == 0) return;

	int32 Retyped = 0;

	for (FNiagaraVariable& Parameter : Collection->GetParameters()) {
		const FNiagaraTypeDefinition* Type = Types.Find(Parameter.GetName());

		if (Type == nullptr) continue;

		Parameter.SetType(*Type);

		/* A default the size of the type it is now, since one written at another size is a value
		 * nothing can read, and the two here that arrived with no default had none to write */
		Parameter.AllocateData();

		Retyped++;
	}

	UNiagaraParameterCollectionInstance* Instance = Collection->GetDefaultInstance();

	if (Instance == nullptr) return;

	FNiagaraParameterStore& Store = Instance->GetParameterStore();

	/* Anything the instance came in holding, kept before the store is taken apart.
	 *
	 * Read at the offsets the export gave, which were written for the sizes the parameters really
	 * are, so each value sits where it says it does once the types are right. */
	const TArray<uint8> Arrived = Store.GetParameterDataArray();

	TMap<FName, TArray<uint8>> Overrides;

	for (const FNiagaraVariableWithOffset& Variable : Store.ReadParameterVariables()) {
		const FNiagaraTypeDefinition* Type = Types.Find(Variable.GetName());

		if (Type == nullptr) continue;

		const int32 Size = Type->GetSize();

		/* One whose bytes are not all there is left to the parameter's own default rather than read
		 * past the end of what came across */
		if (Variable.Offset < 0 || Variable.Offset + Size > Arrived.Num()) continue;

		Overrides.Add(Variable.GetName(), TArray<uint8>(Arrived.GetData() + Variable.Offset, Size));
	}

	/* Laid out again from the collection's own list.
	 *
	 * The instance is made as a subobject the moment the collection is, and squares itself against
	 * a parameter list that is still empty at that point, which leaves its store cleared. What
	 * survives that is the parameters, so the store is built from them, one at a time, with the
	 * engine handing out the offsets. That is what the engine does for a default instance anyway,
	 * and it is what keeps the two from ever disagreeing again.
	 *
	 * A value the instance did come in holding is taken ahead of the parameter's own default, since
	 * that is the instance saying something the collection does not. */
	Store.Empty();

	int32 Kept = 0;

	for (const FNiagaraVariable& Parameter : Collection->GetParameters()) {
		if (const TArray<uint8>* Bytes = Overrides.Find(Parameter.GetName())) {
			Store.SetParameterData(Bytes->GetData(), Parameter, true);

			Kept++;
		} else if (Parameter.IsDataAllocated()) {
			Store.SetParameterData(Parameter.GetData(), Parameter, true);
		} else {
			Store.AddParameter(Parameter, true, false);
		}
	}

	/* Ordered the way the store is searched, which is a binary search and answers wrongly on a list
	 * that is not in that order */
	Store.SortParameters();

	UE_LOG(LogReflection, Display, TEXT("\"%s\" typed %d parameter(s) and laid out %d of them%s"),
		*GetAssetName(), Retyped, Collection->GetParameters().Num(),
		Kept > 0 ? *FString::Printf(TEXT(", %d held by the instance"), Kept) : TEXT(""));
}

void INiagaraParameterCollectionImporter::Validate(UObject* Asset) const {
	UNiagaraParameterCollection* Collection = Cast<UNiagaraParameterCollection>(Asset);

	if (Collection == nullptr) return;

	UNiagaraParameterCollectionInstance* Instance = Collection->GetDefaultInstance();

	if (Instance == nullptr) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The collection has no default instance"),
			FString::Printf(
				TEXT("'%s' keeps its values in an instance beside it, and without one its parameters are names with nothing behind them."),
				*GetAssetName())
		);

		return;
	}

	const FNiagaraParameterStore& Store = Instance->GetParameterStore();

	const int32 Held = Store.GetParameterDataArray().Num();

	/* Every parameter has to fit inside the blob it points into. One that doesn't is the store
	 * disagreeing with itself, and the engine finds it the moment anything reads that parameter. */
	int32 Overruns = 0;

	for (const FNiagaraVariableWithOffset& Variable : Store.ReadParameterVariables()) {
		if (Variable.Offset < 0 || Variable.Offset + Variable.GetSizeInBytes() > Held) {
			Overruns++;
		}
	}

	if (Overruns > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d parameter(s) reach past the end of the store"), Overruns),
			FString::Printf(
				TEXT("'%s' holds parameters whose offset and size put them outside the values it carries, so reading one reads whatever follows."),
				*GetAssetName())
		);
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" carries %d parameter(s) over %d byte(s)%s"),
		*GetAssetName(), Store.ReadParameterVariables().Num(), Held,
		Overruns > 0 ? TEXT(", not all of which fit") : TEXT(""));
}
