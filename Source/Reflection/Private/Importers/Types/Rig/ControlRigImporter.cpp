/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Rig/ControlRigImporter.h"
#include "Importers/Types/Blueprint/BlueprintCompile.h"

#if REFLECTION_CONTROL_RIG

#include "ControlRig.h"
#include "Engine/ControlRigCompatibility.h"
#include "Rigs/RigHierarchyController.h"

#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "RigVMCore/RigVMExternalVariable.h"
#include "RigVMDeveloperTypeUtils.h"

#include "Importers/Types/Blueprint/BlueprintUtilities.h"
#include "Importers/Types/Blueprint/BlueprintVariables.h"
#include "Importers/Types/Rig/RigVMGraph.h"
#include "Utilities/AssetPaths.h"

namespace {
	/* A hierarchy is written by the rig's own serializer rather than as tagged properties, so what
	 * comes out of an export is the number an enum was stored as. The four below are the element
	 * types Reflection can put back; the rest (rigid bodies, references, connectors and sockets)
	 * are reported by the caller rather than guessed at. */
	ERigElementType ToElementType(const FUObjectJsonValueExport& Key) {
		if (!Key.JsonObject.IsValid() || !Key.Has(TEXT("Type"))) {
			return ERigElementType::None;
		}

		const TSharedPtr<FJsonValue> Type = Key.JsonObject->TryGetField(TEXT("Type"));

		/* Tagged properties spell the same enum out, which is what the class default object's
		 * copies of these keys arrive as */
		if (Type.IsValid() && Type->Type == EJson::String) {
			FString Name = Type->AsString();

			if (Name.Contains(TEXT("::"))) {
				Name.Split(TEXT("::"), nullptr, &Name);
			}

			if (Name == TEXT("Bone")) return ERigElementType::Bone;
			if (Name == TEXT("Null") || Name == TEXT("Space")) return ERigElementType::Null;
			if (Name == TEXT("Control")) return ERigElementType::Control;
			if (Name == TEXT("Curve")) return ERigElementType::Curve;

			return ERigElementType::None;
		}

		switch (Key.GetInteger(TEXT("Type"), 0)) {
			case 1: return ERigElementType::Bone;
			case 2: return ERigElementType::Null;
			case 4: return ERigElementType::Control;
			case 8: return ERigElementType::Curve;
			default: return ERigElementType::None;
		}
	}

	FRigElementKey ReadElementKey(const FUObjectJsonValueExport& Key) {
		if (!Key.JsonObject.IsValid() || !Key.Has(TEXT("Name"))) {
			return FRigElementKey();
		}

		const FString Name = Key.GetString(TEXT("Name"));
		const ERigElementType Type = ToElementType(Key);

		if (Name.IsEmpty() || Name == TEXT("None") || Type == ERigElementType::None) {
			return FRigElementKey();
		}

		return FRigElementKey(StringToName(Name), Type);
	}

	double ReadNumber(const FUObjectJsonValueExport& Object, const FString& Field) {
		return Object.Has(Field) ? Object.GetNumber(Field) : 0.0;
	}

	/* One transform out of an element's transform storage.
	 *
	 * Only the initial local one is read. A cooked rig's current pose is wherever the rig happened
	 * to be left, and both global poses are caches the hierarchy fills in as it evaluates: they
	 * come through zeroed, quaternion included, which is not a rotation. */
	bool ReadInitialLocalTransform(const FUObjectJsonValueExport& Storage, FTransform& OutTransform) {
		const FUObjectJsonValueExport Transform = Storage.GetObject(TEXT("Initial")).GetObject(TEXT("Local")).GetObject(TEXT("Transform"));

		if (!Transform.JsonObject.IsValid() || !Transform.Has(TEXT("Rotation"))) {
			return false;
		}

		const FUObjectJsonValueExport Rotation = Transform.GetObject(TEXT("Rotation"));
		const FUObjectJsonValueExport Translation = Transform.GetObject(TEXT("Translation"));
		const FUObjectJsonValueExport Scale = Transform.GetObject(TEXT("Scale3D"));

		const FQuat Quaternion(
			ReadNumber(Rotation, TEXT("X")),
			ReadNumber(Rotation, TEXT("Y")),
			ReadNumber(Rotation, TEXT("Z")),
			ReadNumber(Rotation, TEXT("W"))
		);

		/* Storage the rig never wrote to, which the identity stands in for */
		if (!Quaternion.IsNormalized()) {
			return false;
		}

		OutTransform = FTransform(
			Quaternion,
			FVector(ReadNumber(Translation, TEXT("X")), ReadNumber(Translation, TEXT("Y")), ReadNumber(Translation, TEXT("Z"))),
			FVector(ReadNumber(Scale, TEXT("X")), ReadNumber(Scale, TEXT("Y")), ReadNumber(Scale, TEXT("Z")))
		);

		return true;
	}

	/* A control's limits are written as the storage they sit in rather than through the value that
	 * holds it, so the object handed in here is that storage rather than an FRigControlValue */
	void ReadControlValue(UPropertySerializer* PropertySerializer, const FUObjectJsonValueExport& Json, FRigControlValue& OutValue) {
		if (!Json.JsonObject.IsValid() || !Json.Has(TEXT("Float00"))) {
			return;
		}

		PropertySerializer->DeserializeStruct(
			FRigControlValueStorage::StaticStruct(),
			Json.JsonObject.ToSharedRef(),
			&OutValue.GetRef<FRigControlValueStorage>()
		);
	}

	/* Everything one element of the export says, read once so the order they go in can be decided */
	struct FRigElementDescription {
		FRigElementKey Key;
		FRigElementKey Parent;

		FUObjectJsonValueExport Json;
	};

	/* A reference written as Class'RigElementKey' in /Script/ControlRig, spelled the way the
	 * variable API takes a type: /Script/ControlRig.RigElementKey */
	FString ReadTypeObjectPath(const FUObjectJsonValueExport& Reference) {
		if (!Reference.JsonObject.IsValid() || !Reference.Has(TEXT("ObjectName")) || !Reference.Has(TEXT("ObjectPath"))) {
			return FString();
		}

		FString ObjectName = Reference.GetString(TEXT("ObjectName"));
		FString ObjectPath = Reference.GetString(TEXT("ObjectPath"));

		/* Class'RigElementKey' -> RigElementKey */
		int32 Quote = INDEX_NONE;

		if (ObjectName.FindChar(TEXT('\''), Quote)) {
			ObjectName = ObjectName.Mid(Quote + 1).LeftChop(1);
		}

		/* An asset's path ends in the index of the export it names, which isn't part of its name */
		FString Package;
		FString Index;

		if (ObjectPath.Split(TEXT("."), &Package, &Index, ESearchCase::IgnoreCase, ESearchDir::FromEnd) && Index.IsNumeric()) {
			ObjectPath = Package;
		}

		if (ObjectName.IsEmpty() || ObjectPath.IsEmpty()) {
			return FString();
		}

		return ObjectPath + TEXT(".") + ObjectName;
	}

	/* The CPP type the variable API takes for one property out of a property bag, or an empty
	 * string for a type a rig variable can't be */
	FString ReadPropertyBagType(const FUObjectJsonValueExport& Description) {
		FString ValueType = Description.GetString(TEXT("ValueType"));

		if (ValueType.Contains(TEXT("::"))) {
			ValueType.Split(TEXT("::"), nullptr, &ValueType);
		}

		/* Enums, structs and objects are named by the type they hold, which is the whole of what the
		 * variable API needs to know about them */
		FString Type = ReadTypeObjectPath(Description.GetObject(TEXT("ValueTypeObject")));

		if (Type.IsEmpty()) {
			if (ValueType == TEXT("Bool")) Type = TEXT("bool");
			else if (ValueType == TEXT("Byte")) Type = TEXT("uint8");
			else if (ValueType == TEXT("Int32")) Type = TEXT("int32");
			else if (ValueType == TEXT("Int64")) Type = TEXT("int64");
			else if (ValueType == TEXT("Float")) Type = TEXT("float");
			else if (ValueType == TEXT("Double")) Type = TEXT("double");
			else if (ValueType == TEXT("Name")) Type = TEXT("FName");
			else if (ValueType == TEXT("String")) Type = TEXT("FString");
			else if (ValueType == TEXT("Text")) Type = TEXT("FText");
			else return FString();
		}

		/* Containers were one field and became a list of them, outermost first. A rig variable can
		 * be an array and nothing else. */
		TArray<FString> Containers;

		const FUObjectJsonValueExport ContainerTypes = Description.GetObject(TEXT("ContainerTypes"));

		if (ContainerTypes.JsonObject.IsValid()) {
			const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;

			if (ContainerTypes.JsonObject->TryGetArrayField(TEXT("Types"), Listed)) {
				for (const TSharedPtr<FJsonValue>& Container : *Listed) {
					if (Container.IsValid() && Container->Type == EJson::String) {
						Containers.Add(Container->AsString());
					}
				}
			}
		}

		if (Containers.Num() == 0 && Description.Has(TEXT("ContainerType"))) {
			Containers.Add(Description.GetString(TEXT("ContainerType")));
		}

		int32 Arrays = 0;

		for (FString Container : Containers) {
			if (Container.Contains(TEXT("::"))) {
				Container.Split(TEXT("::"), nullptr, &Container);
			}

			if (Container == TEXT("None")) continue;
			if (Container != TEXT("Array")) return FString();

			Arrays++;
		}

		if (Arrays > 1) return FString();

		return Arrays == 1 ? FString::Printf(TEXT("TArray<%s>"), *Type) : Type;
	}
}

UObject* IControlRigImporter::CreateAsset(UObject* CreatedAsset) {
	/* Reflected over an earlier run: the asset is already there, and the import rebuilds it in
	 * place rather than creating a second one beside it */
	if (UControlRigBlueprint* ExistingBlueprint = LoadObject<UControlRigBlueprint>(nullptr, *GetPackage()->GetPathName())) {
		return IImporter::CreateAsset(ExistingBlueprint);
	}

	/* A runtime asset has no class, and every rig cooked to one runs as a plain ControlRig */
	UClass* ParentClass = IsRuntimeAsset() ? UControlRig::StaticClass() : GetAssetClass();

	/* A rig built on a class of the game's own is one this project doesn't have. The rest of the
	 * asset has nothing to do with that class, so it is built on the one every rig derives from
	 * and the missing parent is reported rather than failing the import. */
	if (ParentClass == nullptr || !ParentClass->IsChildOf(UControlRig::StaticClass()) || !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass)) {
		FImportIssues::Report(
			EImportIssue::MissingClass,
			TEXT("The rig's parent class isn't in this project"),
			TEXT("The rig was rebuilt on ControlRig instead. Anything the parent class brought with it, including its own variables, is not part of the asset.")
		);

		ParentClass = UControlRig::StaticClass();
	}

	/* The factory is what knows to give the blueprint its rig graph, which nothing else creates */
	UFactory* Factory = RigBlueprintFactory(ParentClass);

	UObject* Blueprint = Factory != nullptr ? Factory->FactoryCreateNew(
		UControlRigBlueprint::StaticClass(),
		GetPackage(),
		FName(*GetAssetName()),
		RF_Public | RF_Standalone,
		nullptr,
		GWarn
	) : nullptr;

	return IImporter::CreateAsset(Blueprint);
}

bool IControlRigImporter::Import() {
	UControlRigBlueprint* Blueprint = Create<UControlRigBlueprint>();
	if (Blueprint == nullptr) return false;

	ConstructHierarchy(Blueprint);
	ConstructSettings(Blueprint);

	/* The variables have to exist before their defaults can land anywhere, and they only become
	 * properties on the generated class once the blueprint has been compiled again. The graph
	 * below reads and writes them, so they also have to be there before it is built. */
	if (ConstructVariables(Blueprint) > 0) {
		CompileBlueprintGuarded(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

		ApplyVariableDefaults(Blueprint);
	}

	ConstructGraph(Blueprint);

	/* The hierarchy is edited on the blueprint, and every rig built from it holds a copy of its
	 * own that has to be told */
	Blueprint->PropagateHierarchyFromBPToInstances();
	Blueprint->RecompileVM();

	return OnAssetCreation(Blueprint);
}

int32 IControlRigImporter::ConstructHierarchy(UControlRigBlueprint* Blueprint) const {
	URigHierarchy* Hierarchy = RigHierarchyOf(Blueprint);
	URigHierarchyController* Controller = Blueprint->GetHierarchyController();

	if (Hierarchy == nullptr || Controller == nullptr) return 0;

	/* The hierarchy is a subobject of the class default object rather than an export of its own,
	 * so it is reached through what the default object points at. A runtime asset holds it
	 * directly, under a name of its own. */
	const FUObjectJsonValueExport RigProperties = GetRigProperties();
	FUObjectExport* HierarchyExport = FUObjectExport::EmptyExport();

	if (RigProperties.JsonObject.IsValid()) {
		HierarchyExport = GetContainer()->GetExportByObjectPath(RigProperties.GetObject(IsRuntimeAsset() ? TEXT("Hierarchy") : TEXT("DynamicHierarchy")));
	}

	if (HierarchyExport->IsJsonInvalid()) {
		HierarchyExport = GetContainer()->FindByType(FName(TEXT("RigHierarchy")));
	}

	const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;

	/* A runtime asset whose construction event builds the hierarchy is cooked with an empty one */
	if (IsRuntimeAsset() && (HierarchyExport->IsJsonInvalid() || !HierarchyExport->JsonObject->TryGetArrayField(TEXT("Elements"), Elements) || Elements->Num() == 0)) {
		return ImportHierarchyFromSkeleton(Blueprint);
	}

	if (HierarchyExport->IsJsonInvalid() || !HierarchyExport->JsonObject->TryGetArrayField(TEXT("Elements"), Elements)) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The rig has no hierarchy to rebuild"),
			TEXT("The export carries no RigHierarchy, so the rig comes back with no bones, nulls or controls on it.")
		);

		return 0;
	}

	/* Reflected over an earlier run, the hierarchy still holds that run's elements. Leaves first,
	 * since removing a parent takes its children with it. */
	if (Hierarchy->Num() > 0) {
		TArray<FRigElementKey> ExistingKeys = Hierarchy->GetAllKeys(true);

		for (int32 Index = ExistingKeys.Num() - 1; Index >= 0; --Index) {
			Controller->RemoveElement(ExistingKeys[Index], false, false);
		}
	}

	TArray<FRigElementDescription> Descriptions;
	Descriptions.Reserve(Elements->Num());

	int32 Unsupported = 0;

	for (const TSharedPtr<FJsonValue>& Element : *Elements) {
		if (!Element.IsValid() || Element->Type != EJson::Object) continue;

		const FUObjectJsonValueExport Json(Element->AsObject());

		FRigElementDescription Description;
		Description.Json = Json;
		Description.Key = ReadElementKey(Json.GetObject(TEXT("LoadedKey")));

		/* Bones name their parent outright. Everything else in a rig can be parented to several
		 * elements at once, and holds pointers to them rather than their keys: what a cooked
		 * export has to say about those is reported below rather than guessed at. */
		Description.Parent = ReadElementKey(Json.GetObject(TEXT("ParentKey")));

		if (!Description.Parent.IsValid()) {
			/* The constraints sit in a struct of their own, which holds the list beside the count
			 * and the weights. Older exports wrote the list itself under the same name, and named
			 * the parent by the element rather than by its key. */
			const FUObjectJsonValueExport Constraints = Json.GetObject(TEXT("ParentConstraints"));

			TArray<FUObjectJsonValueExport> Listed;

			if (Constraints.JsonObject.IsValid() && Constraints.Has(TEXT("ParentConstraints"))) {
				Listed = Constraints.GetArray(TEXT("ParentConstraints"));
			} else if (!Constraints.JsonObject.IsValid() && Json.Has(TEXT("ParentConstraints"))) {
				Listed = Json.GetArray(TEXT("ParentConstraints"));
			}

			/* The first is the one the element hangs off; the rest are weights the graph drives */
			if (Listed.Num() > 0) {
				Description.Parent = ReadElementKey(Listed[0].GetObject(TEXT("ParentKey")));

				if (!Description.Parent.IsValid()) {
					Description.Parent = ReadElementKey(Listed[0].GetObject(TEXT("ParentElement")).GetObject(TEXT("Key")));
				}

				if (!Description.Parent.IsValid()) {
					Description.Parent = ReadElementKey(Listed[0].GetObject(TEXT("ParentElement")));
				}
			}
		}

		if (!Description.Key.IsValid()) {
			Unsupported++;

			continue;
		}

		Descriptions.Add(Description);
	}

	auto AddElement = [this, Controller](const FRigElementDescription& Description) -> bool {
		FTransform Transform = FTransform::Identity;
		ReadInitialLocalTransform(Description.Json.GetObject(TEXT("PoseStorage")), Transform);

		switch (Description.Key.Type) {
			case ERigElementType::Bone: {
				/* User bones are the ones added to the rig itself rather than imported off a
				 * skeleton, which is the difference the editor draws them by */
				const ERigBoneType BoneType = Description.Json.GetInteger(TEXT("BoneType"), 0) == 1
					? ERigBoneType::User
					: ERigBoneType::Imported;

				return Controller->AddBone(Description.Key.Name, Description.Parent, Transform, false, BoneType, false, false).IsValid();
			}

			case ERigElementType::Null: {
				return Controller->AddNull(Description.Key.Name, Description.Parent, Transform, false, false, false).IsValid();
			}

			case ERigElementType::Control: {
				FRigControlSettings Settings;

				/* A control's settings were split in two: what the hierarchy is shaped by, and
				 * what the control is drawn with. An export keeping both under one name still
				 * reads, since the fields are the same either way. */
				FUObjectJsonValueExport SettingsJson = Description.Json.GetObject(TEXT("Settings"));

				if (!SettingsJson.JsonObject.IsValid()) {
					SettingsJson = Description.Json.GetObject(TEXT("TopologyControlSettings"));
				}

				if (SettingsJson.JsonObject.IsValid()) {
					GetPropertySerializer()->DeserializeStruct(FRigControlSettings::StaticStruct(), SettingsJson.JsonObject.ToSharedRef(), &Settings);

					ReadControlValue(GetPropertySerializer(), SettingsJson.GetObject(TEXT("MinimumValue")), Settings.MinimumValue);
					ReadControlValue(GetPropertySerializer(), SettingsJson.GetObject(TEXT("MaximumValue")), Settings.MaximumValue);
				}

				const FUObjectJsonValueExport ShapeJson = Description.Json
					.GetObject(TEXT("ControlSettingsShapeData"))
					.GetObject(TEXT("ShapeSettings"));

				if (ShapeJson.JsonObject.IsValid()) {
					GetPropertySerializer()->DeserializeStruct(FRigControlSettings::StaticStruct(), ShapeJson.JsonObject.ToSharedRef(), &Settings);
				}

				FTransform Offset = FTransform::Identity;
				FTransform Shape = FTransform::Identity;

				ReadInitialLocalTransform(Description.Json.GetObject(TEXT("Offset")), Offset);
				ReadInitialLocalTransform(Description.Json.GetObject(TEXT("Shape")), Shape);

				/* A control's value is kept as the transform it puts the rig in, and what that
				 * transform means is up to the type of control: the conversion is the rig's */
				FRigControlValue Value;
				Value.SetFromTransform(Transform, Settings.ControlType, Settings.PrimaryAxis);

				return Controller->AddControl(Description.Key.Name, Description.Parent, Settings, Value, Offset, Shape, false, false).IsValid();
			}

			case ERigElementType::Curve: {
				const float Value = static_cast<float>(ReadNumber(Description.Json, TEXT("Value")));

				return Controller->AddCurve(Description.Key.Name, Value, false, false).IsValid();
			}

			default: {
				return false;
			}
		}
	};

	int32 Added = 0;
	int32 Orphaned = 0;

	/* An element can only go in once its parent is there to take it. The export lists a hierarchy
	 * in the order it was built, parents first, but that is the rig's own ordering rather than
	 * anything promised, so the passes below keep going for as long as one of them adds something. */
	TArray<const FRigElementDescription*> Remaining;
	Remaining.Reserve(Descriptions.Num());

	for (const FRigElementDescription& Description : Descriptions) {
		Remaining.Add(&Description);
	}

	while (Remaining.Num() > 0) {
		int32 AddedThisPass = 0;

		for (int32 Index = 0; Index < Remaining.Num();) {
			const FRigElementDescription& Description = *Remaining[Index];

			if (Description.Parent.IsValid() && !Hierarchy->Contains(Description.Parent)) {
				Index++;

				continue;
			}

			if (AddElement(Description)) {
				Added++;
				AddedThisPass++;
			}

			Remaining.RemoveAt(Index);
		}

		/* Everything left names a parent that never turns up, so it goes in unparented */
		if (AddedThisPass == 0) {
			for (const FRigElementDescription* Description : Remaining) {
				FRigElementDescription Unparented = *Description;
				Unparented.Parent = FRigElementKey();

				if (AddElement(Unparented)) {
					Added++;
					Orphaned++;
				}
			}

			break;
		}
	}

	/* Counted rather than reported one by one: a rig's controls are all in the same position */
	int32 Parentless = 0;

	for (const FRigElementDescription& Description : Descriptions) {
		if (!Description.Parent.IsValid() && Description.Key.Type != ERigElementType::Curve) {
			Parentless++;
		}
	}

	if (Parentless > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("Some rig elements came in without their parent"),
			FString::Printf(
				TEXT("%d of %d elements name no parent in the export. A rig element that can have several parents stores pointers to them rather than their names, and an export written from a cooked asset has nothing to resolve those pointers against, so they land at the top of the hierarchy."),
				Parentless,
				Descriptions.Num()
			)
		);
	}

	if (Orphaned > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("Some rig elements name a parent the export doesn't have"),
			FString::Printf(TEXT("%d were added at the top of the hierarchy instead."), Orphaned)
		);
	}

	if (Unsupported > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The hierarchy holds elements Reflection can't rebuild"),
			FString::Printf(TEXT("%d of them, which are neither bones, nulls, controls nor curves."), Unsupported)
		);
	}

	return Added;
}

int32 IControlRigImporter::ConstructVariables(UControlRigBlueprint* Blueprint) {
	if (IsRuntimeAsset()) {
		return ConstructRuntimeVariables(Blueprint);
	}

	/* A rig that declared nothing of its own declares nothing here either */
	return FBlueprintVariables::Construct(Blueprint, FBlueprintVariables::GetDeclared(GetAssetExport(), GetContainer()));
}

void IControlRigImporter::ConstructGraph(UControlRigBlueprint* Blueprint) const {
	/* The VM is a subobject of the class default object, the same way the hierarchy is, and a
	 * runtime asset holds it under the same name */
	const FUObjectJsonValueExport RigProperties = GetRigProperties();
	FUObjectExport* VirtualMachineExport = FUObjectExport::EmptyExport();

	if (RigProperties.JsonObject.IsValid()) {
		VirtualMachineExport = GetContainer()->GetExportByObjectPath(RigProperties.GetObject(TEXT("VM")));
	}

	if (VirtualMachineExport->IsJsonInvalid()) {
		VirtualMachineExport = GetContainer()->FindByType(FName(TEXT("RigVM")));
	}

	if (VirtualMachineExport->IsJsonInvalid()) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The rig has no graph to rebuild"),
			TEXT("The export carries no RigVM, which is the only thing a cooked rig keeps of its graph, so the rig comes back with an empty Forwards Solve.")
		);

		return;
	}

	FRigVMGraphReconstruction Reconstruction(Blueprint, GetPropertySerializer());

	if (!Reconstruction.Build(VirtualMachineExport->AsValueExport())) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The rig's graph couldn't be rebuilt"),
			TEXT("The VM carries no bytecode to read the graph back out of.")
		);

		return;
	}

	const FRigVMGraphStats& Stats = Reconstruction.GetStats();

	UE_LOG(LogReflection, Log, TEXT("\"%s\" rebuilt %d nodes, %d links and %d pin defaults from %d instructions, across %d function(s) called %d time(s)"),
		*GetAssetName(), Stats.Nodes, Stats.Links, Stats.Defaults, Stats.Instructions, Stats.Functions, Stats.FunctionCalls);

	/* Collapsed nodes and function references are compiled away long before the VM exists, so what
	 * comes back is the graph the VM runs rather than the one that was drawn */
	FImportIssues::Report(
		EImportIssue::Data,
		TEXT("The rig's graph was rebuilt from its bytecode"),
		FString::Printf(
			TEXT("%d nodes, %d links and %d pin defaults, out of %d instructions, with %d function(s) called %d time(s). A cook keeps only the compiled VM, so collapsed nodes come back as the nodes they were compiled into, and comments and node positions are gone."),
			Stats.Nodes,
			Stats.Links,
			Stats.Defaults,
			Stats.Instructions,
			Stats.Functions,
			Stats.FunctionCalls
		)
	);

	if (Stats.MissingFunctions.Num() > 0) {
		FImportIssues::Report(
			EImportIssue::MissingClass,
			TEXT("The rig uses nodes this engine build doesn't have"),
			FString::Printf(
				TEXT("%d of them, starting with \"%s\". Either the plugin that declares the node isn't enabled, or the engine spells it differently to the one the rig was cooked from. They are left out of the graph, along with anything that was linked through them."),
				Stats.MissingFunctions.Num(),
				*Stats.MissingFunctions[0]
			)
		);
	}

	if (Stats.MismatchedFunctions.Num() > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("Some of the rig's nodes have pins this engine build doesn't"),
			FString::Printf(
				TEXT("%d of them, starting with \"%s\", %d pins in total. The game's engine gave those nodes pins this one doesn't declare: the node is rebuilt with the pins both versions share, and whatever fed the rest is left unlinked."),
				Stats.MismatchedFunctions.Num(),
				*Stats.MismatchedFunctions[0],
				Stats.DroppedPins
			)
		);
	}

	if (Stats.FailedNodes > 0 || Stats.FailedLinks > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("Parts of the rig's graph wouldn't go back together"),
			FString::Printf(TEXT("%d nodes and %d links the graph refused."), Stats.FailedNodes, Stats.FailedLinks)
		);
	}
}

void IControlRigImporter::ConstructSettings(UControlRigBlueprint* Blueprint) const {
	/* The mesh the rig is previewed and authored against. Not part of what the rig does, but a rig
	 * opened without one has nothing to show its controls on. */
	const FUObjectJsonValueExport PreviewMesh = GetAssetDataAsValue().GetObject(TEXT("PreviewSkeletalMesh"));

	if (PreviewMesh.Has(TEXT("AssetPathName"))) {
		const FString AssetPathName = PreviewMesh.GetString(TEXT("AssetPathName"));

		FString PackagePath;
		FString AssetName;

		if (AssetPathName.Split(TEXT("."), &PackagePath, &AssetName)) {
			TObjectPtr<UObject> Mesh = LoadObjectByPath<UObject>(ToEditorPackagePath(PackagePath) + TEXT(".") + AssetName);

			if (Mesh == nullptr) {
				Mesh = DownloadWrapper(Mesh, TEXT("SkeletalMesh"), AssetName, PackagePath);
			}

			if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh)) {
				Blueprint->SetPreviewMesh(SkeletalMesh);
			}
		}
	}

#if WITH_EDITORONLY_DATA
	/* The libraries the shape names on the rig's controls are looked up in. A rig created from
	 * scratch starts with the engine's own, which is what most rigs are still on. */
	const FUObjectJsonValueExport Properties = GetRigProperties();

	if (Properties.JsonObject.IsValid()) {
		if (Properties.Has(TEXT("ShapeLibraries"))) {
			TArray<TSoftObjectPtr<UControlRigShapeLibrary>> ShapeLibraries;

			for (const FUObjectJsonValueExport& ShapeLibrary : Properties.GetArray(TEXT("ShapeLibraries"))) {
				if (!ShapeLibrary.Has(TEXT("AssetPathName"))) continue;

				const FString AssetPathName = ShapeLibrary.GetString(TEXT("AssetPathName"));
				if (AssetPathName.IsEmpty() || AssetPathName == TEXT("None")) continue;

				ShapeLibraries.Add(TSoftObjectPtr<UControlRigShapeLibrary>(FSoftObjectPath(AssetPathName)));
			}

			if (ShapeLibraries.Num() > 0) {
				Blueprint->ShapeLibraries = ShapeLibraries;
			}
		}
	}
#endif
}

void IControlRigImporter::ApplyVariableDefaults(const UControlRigBlueprint* Blueprint) const {
	UClass* GeneratedClass = Blueprint->GeneratedClass;
	if (GeneratedClass == nullptr) return;

	TSharedPtr<FJsonObject> Properties;

	/* A runtime asset keeps its defaults as the value of its property bag, by variable name */
	if (IsRuntimeAsset()) {
		Properties = GetAssetDataAsValue().GetObject(TEXT("Variables")).GetObject(TEXT("Value")).JsonObject;
	} else {
		FUObjectExport* DefaultObjectExport = GetClassDefaultObject(GetContainer(), GetAssetDataAsValue());

		if (DefaultObjectExport->IsJsonValid() && DefaultObjectExport->JsonObject->HasField(TEXT("Properties"))) {
			Properties = DefaultObjectExport->GetProperties();
		}
	}

	if (!Properties.IsValid()) return;

	/* Only the variables this import added. The rest of what a rig's default object carries is the
	 * rig itself, its hierarchy and its VM, which are built rather than deserialized: writing the
	 * export's copies of those over them would leave the asset pointing at nothing. */
	const TSharedPtr<FJsonObject> Defaults = MakeShared<FJsonObject>();

	for (const FBPVariableDescription& Variable : Blueprint->NewVariables) {
		const FString Name = Variable.VarName.ToString();

		if (!Properties->HasField(Name)) continue;

		Defaults->SetField(StringToJsonKey(Name), Properties->TryGetField(Name));
	}

	if (Defaults->Values.Num() == 0) return;

	GetObjectSerializer()->DeserializeObjectProperties(Defaults, GeneratedClass->GetDefaultObject());
}

bool IControlRigImporter::IsRuntimeAsset() const {
	return GetAssetType() == TEXT("ControlRigRuntimeAsset");
}

FUObjectJsonValueExport IControlRigImporter::GetRigProperties() const {
	if (IsRuntimeAsset()) {
		return GetAssetDataAsValue();
	}

	FUObjectExport* DefaultObjectExport = GetClassDefaultObject(GetContainer(), GetAssetDataAsValue());

	if (DefaultObjectExport->IsJsonInvalid() || !DefaultObjectExport->JsonObject->HasField(TEXT("Properties"))) {
		return FUObjectJsonValueExport();
	}

	return DefaultObjectExport->GetPropertiesAsValue();
}

int32 IControlRigImporter::ConstructRuntimeVariables(UControlRigBlueprint* Blueprint) {
	const FUObjectJsonValueExport Bag = GetAssetDataAsValue().GetObject(TEXT("Variables"));

	if (!Bag.JsonObject.IsValid() || !Bag.Has(TEXT("PropertyDescs"))) {
		return 0;
	}

	int32 Added = 0;
	TArray<FString> Refused;

	/* The graph reads a variable by its index into this list, so they go in the order the bag
	 * declares them, and one that can't go in is reported rather than skipped over quietly */
	for (const FUObjectJsonValueExport& Description : Bag.GetArray(TEXT("PropertyDescs"))) {
		const FString Name = Description.GetString(TEXT("Name"));
		if (Name.IsEmpty()) continue;

		const FString Type = ReadPropertyBagType(Description);

		/* The rig's own AddMemberVariable compiles the blueprint after every variable, with garbage
		 * collection on, and that frees the serializers the rest of this import reads through. The
		 * pin type it would have built is built here instead, and the one compile the caller does
		 * once they are all in stands for the lot. */
		const FRigVMExternalVariable Variable = Type.IsEmpty()
			? FRigVMExternalVariable()
			: RigVMTypeUtils::ExternalVariableFromCPPTypePath(FName(*Name), Type, false, false);

		const FEdGraphPinType PinType = Variable.IsValid(true) ? RigVMTypeUtils::PinTypeFromExternalVariable(Variable) : FEdGraphPinType();

		if (!PinType.PinCategory.IsValid() || PinType.PinCategory.IsNone() || !FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*Name), PinType)) {
			Refused.Add(Name);

			continue;
		}

		Added++;
	}

	if (Refused.Num() > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("Some of the rig's variables couldn't be declared"),
			FString::Printf(
				TEXT("%d of them, starting with \"%s\". Their type is one a rig variable can't hold or one this project doesn't have, and the graph's reads of every variable after them may land on the wrong one."),
				Refused.Num(),
				*Refused[0]
			)
		);
	}

	return Added;
}

int32 IControlRigImporter::ImportHierarchyFromSkeleton(UControlRigBlueprint* Blueprint) const {
	URigHierarchyController* Controller = Blueprint->GetHierarchyController();
	if (Controller == nullptr) return 0;

	auto LoadSkeleton = [](const FUObjectJsonValueExport& Reference) -> USkeleton* {
		if (!Reference.JsonObject.IsValid() || !Reference.Has(TEXT("AssetPathName"))) return nullptr;

		FString PackagePath;
		FString AssetName;

		if (!Reference.GetString(TEXT("AssetPathName")).Split(TEXT("."), &PackagePath, &AssetName)) return nullptr;

		TObjectPtr<UObject> Skeleton = LoadObjectByPath<UObject>(ToEditorPackagePath(PackagePath) + TEXT(".") + AssetName);

		if (Skeleton == nullptr) {
			Skeleton = DownloadWrapper(Skeleton, TEXT("Skeleton"), AssetName, PackagePath);
		}

		return Cast<USkeleton>(Skeleton);
	};

	const FUObjectJsonValueExport Properties = GetAssetDataAsValue();

	USkeleton* HierarchySkeleton = LoadSkeleton(Properties.GetObject(TEXT("SourceHierarchyImport")));
	USkeleton* CurveSkeleton = LoadSkeleton(Properties.GetObject(TEXT("SourceCurveImport")));

	if (HierarchySkeleton == nullptr) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The rig has no hierarchy to rebuild"),
			TEXT("The runtime asset was cooked with an empty hierarchy, and the skeleton it was authored against couldn't be loaded to import one from. The rig's construction event still builds its own elements when it runs.")
		);

		return 0;
	}

	/* Replacing and removing rather than adding to what is there, which over an earlier run is
	 * that run's import */
	int32 Imported = Controller->ImportBones(HierarchySkeleton, NAME_None, true, true, false, false, false).Num();

	if (CurveSkeleton != nullptr) {
		Imported += Controller->ImportCurves(CurveSkeleton, NAME_None, false, false, false).Num();
	}

	FImportIssues::Report(
		EImportIssue::Data,
		TEXT("The rig's hierarchy was imported from its skeleton"),
		FString::Printf(
			TEXT("A runtime asset is cooked with the hierarchy its construction event builds left empty, so the %d bones and curves of the skeleton it was authored against stand in for it. Anything the construction event adds on top of those is added again each time the rig runs."),
			Imported
		)
	);

	return Imported;
}

#endif
