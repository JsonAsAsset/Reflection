/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BlueprintImporter.h"

#include "KismetCompilerModule.h"
#include "MovieScene.h"
#include "WidgetBlueprint.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/MovieSceneWidgetMaterialTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SimpleConstructionScript.h"
#include "Interfaces/Interface_PreviewMeshProvider.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#if ENGINE_UE5
#include "MVVM/ViewModels/ObjectBindingModel.h"
#endif

#include "Engine/SCS_Node.h"
#include "Importers/Constructor/Graph/RigHierarchyBuilder.h"
#include "Importers/Constructor/Graph/RigVMGraphBuilder.h"
#include "Utilities/BlueprintUtilities.h"

UObject* IBlueprintImporter::CreateAsset(UObject* CreatedAsset) {
	UClass* Class = GetAssetClass();
	
	if (!Class) {
		AppendNotification(
			FText::FromString("Failed to Resolve Parent Class"),
			FText::FromString("The Blueprint's parent class could not be found or loaded. Verify that the class is defined and available when reflecting."),
			2.0f,
			SNotificationItem::CS_Fail,
			true,
			350.0f
		);
		
		return nullptr;
	}
	
	/* Find the blueprint class and generated class */
	UClass* BlueprintClass = nullptr, *GeneratedClass = nullptr;

	GetBlueprintTypesForExport(GetAssetType(), Class, BlueprintClass, GeneratedClass);

	/* Propagate blueprint defaults if it already exists */
	if (const UBlueprint* ExistingBlueprint = LoadObject<UBlueprint>(nullptr, *GetPackage()->GetPathName())) {
		UBlueprintGeneratedClass* BlueprintGeneratedClass = Cast<UBlueprintGeneratedClass>(ExistingBlueprint->GeneratedClass);
		FBlueprintEditorUtils::PropagateParentBlueprintDefaults(BlueprintGeneratedClass);

		/* Return GeneratedClass instead of UBlueprint* */
		return IImporter::CreateAsset(BlueprintGeneratedClass);
	}

	const UBlueprint* CreatedBlueprint = FKismetEditorUtilities::CreateBlueprint(
		Class,
		GetPackage(),
		FName(*GetAssetName()),
		GetBlueprintType(Class),
		BlueprintClass,
		GeneratedClass
	);

	if (!CreatedBlueprint) return nullptr;

	/* Return GeneratedClass instead of UBlueprint* */
	return IImporter::CreateAsset(CreatedBlueprint->GeneratedClass);
}

bool IBlueprintImporter::Import() {
	const UBlueprintGeneratedClass* BlueprintGeneratedClass = Create<UBlueprintGeneratedClass>();
	if (!BlueprintGeneratedClass) return false;

	/* Update Blueprint Reference for sub functions */
	Blueprint = UBlueprint::GetBlueprintFromClass(BlueprintGeneratedClass);
	if (!Blueprint) return false;

	/* Deserialize Generated Class (blueprint defaults) */
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
	FUObjectExport* ClassDefaultObjectExport = GetClassDefaultObject(GetContainer(), GetAssetDataAsValue());
	ClassDefaultObjectExport->Object = GeneratedClass;

	GetObjectSerializer()->DeserializeObjectProperties(ClassDefaultObjectExport->GetProperties(), GeneratedClass->GetDefaultObject());

	/* Experimental (for now) spawning */
	GetObjectSerializer()->bUseExperimentalSpawning = true;

	PropagateDefaultsToBlueprint();
	AssignPreviewMesh();

	/* Neither a rig's hierarchy nor its graph survives cooking as reflected data, so both are replayed through
	 * the engine's own editing APIs. The hierarchy goes first, since the graph addresses its elements by name. */
	FRigHierarchyBuilder(Blueprint, GetContainer(), GetPropertySerializer()).Build();
	FRigVMGraphBuilder(Blueprint, GetContainer(), GetPropertySerializer()).Build();

	ConstructScript();
	ConstructWidgetTree();

	return OnAssetCreation(Blueprint);
}

/*
 * Editor-only blueprint data doesn't survive cooking, so for families that keep their authoring data in
 * subobjects (a Control Rig's hierarchy, for instance) the only copy left is the one hanging off the generated
 * CDO. Those blueprints compile their own copy back down into the CDO, so the trip is made in reverse here:
 * every subobject the blueprint owns is filled from the CDO subobject of the same class. Matching on class
 * instead of property name keeps this free of any per-asset-type knowledge.
 */
void IBlueprintImporter::PropagateDefaultsToBlueprint() const {
	UObject* DefaultObject = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
	if (DefaultObject == nullptr) return;

	for (TFieldIterator<FObjectProperty> PropertyIterator(Blueprint->GetClass()); PropertyIterator; ++PropertyIterator) {
		FObjectProperty* Property = *PropertyIterator;

		/* UBlueprint's own fields are the importer's business, only what a subclass added is in scope */
		if (UBlueprint::StaticClass()->IsChildOf(Property->GetOwnerClass())) continue;

		UObject* Existing = Property->GetObjectPropertyValue_InContainer(Blueprint);

		/* Anything outered elsewhere is a reference to another asset, not authoring data */
		if (Existing == nullptr || Existing->GetOuter() != Blueprint) continue;

		UObject* Source = nullptr;
		bool Ambiguous = false;

		for (TFieldIterator<FObjectProperty> SourceIterator(DefaultObject->GetClass()); SourceIterator; ++SourceIterator) {
			UObject* Value = SourceIterator->GetObjectPropertyValue_InContainer(DefaultObject);

			if (Value == nullptr || Value->GetClass() != Existing->GetClass()) continue;

			/* More than one candidate means the class alone doesn't identify it, so leave it alone */
			if (Source != nullptr) {
				Ambiguous = true;
				break;
			}

			Source = Value;
		}

		if (Source == nullptr || Ambiguous) continue;

		const FName SubobjectName = Existing->GetFName();
		MoveToTransientPackageAndRename(Existing);

		Property->SetObjectPropertyValue_InContainer(Blueprint, DuplicateObject(Source, Blueprint, SubobjectName));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
}

/*
 * A preview mesh is editor-only, so a cooked rig or anim blueprint arrives with nothing to pose. Assets that
 * take one advertise it through IInterface_PreviewMeshProvider, which is all the type information needed here:
 * the mesh an asset was authored against sits next to it far more often than not, so the first skeletal mesh in
 * the same folder is used.
 */
void IBlueprintImporter::AssignPreviewMesh() const {
	IInterface_PreviewMeshProvider* PreviewMeshProvider = Cast<IInterface_PreviewMeshProvider>(Blueprint);

	if (PreviewMeshProvider == nullptr || PreviewMeshProvider->GetPreviewMesh() != nullptr) return;

	const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");

	FARFilter Filter;
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(FName(*FPackageName::GetLongPackagePath(Blueprint->GetOutermost()->GetName())));

#if UE5_1_BEYOND
	Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
#else
	Filter.ClassNames.Add(USkeletalMesh::StaticClass()->GetFName());
#endif

	TArray<FAssetData> Assets;
	AssetRegistryModule.Get().GetAssets(Filter, Assets);

	for (const FAssetData& Asset : Assets) {
		if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset.GetAsset())) {
			PreviewMeshProvider->SetPreviewMesh(SkeletalMesh);
			return;
		}
	}
}

void IBlueprintImporter::ConstructScript() const {
	if (!GetAssetDataAsValue().Has("SimpleConstructionScript")) return;
	
	UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);

	/* Destroy Construction Script */
	if (USimpleConstructionScript* PreviousSimpleConstructionScript = GeneratedClass->SimpleConstructionScript; PreviousSimpleConstructionScript != nullptr) {
		for (USCS_Node* Node : PreviousSimpleConstructionScript->GetAllNodes()) {
			MoveToTransientPackageAndRename(Node->ComponentTemplate);
		}
		
		MoveToTransientPackagesAndRename({
			PreviousSimpleConstructionScript,
			Blueprint->SimpleConstructionScript
		});
	}

	FUObjectExport* Export = GetContainer()->GetExportByObjectPath(GetAssetDataAsValue().GetObject("SimpleConstructionScript"));

	/* Spawn the new Construction Script */
	USimpleConstructionScript* SimpleConstructionScript =
		Cast<USimpleConstructionScript>(
			GetObjectSerializer()->SpawnExport(Export)
		);

	/* Update SimpleConstructionScript on the Blueprint */
	Blueprint->SimpleConstructionScript = SimpleConstructionScript;
	GeneratedClass->SimpleConstructionScript = SimpleConstructionScript;

	/* Engine Ensures */
	SimpleConstructionScript->FixupRootNodeParentReferences();
	SimpleConstructionScript->ValidateSceneRootNodes();
}

class UWidgetTreeAccessor final : public UWidgetTree {
public:

#if ENGINE_UE5
	TArray<TObjectPtr<UWidget>> GetWidgets() {
#else
	TArray<UWidget*> GetWidgets() {
#endif
		return AllWidgets;
	}
};

void IBlueprintImporter::ConstructWidgetTree() {
	if (!GetAssetDataAsValue().Has("WidgetTree")) return;

	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint);
	
	for (UWidget* Widget : Cast<UWidgetTreeAccessor>(WidgetBlueprint->WidgetTree)->GetWidgets()) {
		MoveToTransientPackageAndRename(Widget);
	}

	WidgetBlueprint->WidgetTree->PostLoad();

	for (UWidgetAnimation* WidgetAnimation : WidgetBlueprint->Animations) {
		MoveToTransientPackageAndRename(WidgetAnimation);
	}

	WidgetBlueprint->Animations.Empty();
	
	FUObjectExport* ClassDefaultObjectExport = GetClassDefaultObject(GetContainer(), GetAssetDataAsValue());
	ClassDefaultObjectExport->Object = WidgetBlueprint;
	SetAsset(WidgetBlueprint);

	MoveToTransientPackageAndRename(WidgetBlueprint->WidgetTree->RootWidget);
	WidgetBlueprint->WidgetTree->RootWidget = nullptr;

	FUObjectExport* Export;

	if (GetAssetDataAsValue().Has("TemplateAsset")) {
		FUObjectExport* TemplateAsset = GetContainer()->GetExportByObjectPath(GetAssetDataAsValue().GetObject("TemplateAsset"));
		Export = GetContainer()->GetExportByObjectPath(TemplateAsset->GetPropertiesAsValue().GetObject("WidgetTree"));
	} else {
		Export = GetContainer()->GetExportByObjectPath(GetAssetDataAsValue().GetObject("WidgetTree"));
	}
	
	Export->Object = WidgetBlueprint->WidgetTree;
	GetObjectSerializer()->SpawnExport(Export, true);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	GetContainer()->ExportsLoop(GetAssetDataAsValue().GetArray("Animations"), [this, WidgetBlueprint](FUObjectExport* DirectExport) {
		if (UObject* Object = GetObjectSerializer()->SpawnExport(DirectExport)) {
			UWidgetAnimation* WidgetAnimation = Cast<UWidgetAnimation>(Object);
		
			WidgetBlueprint->Animations.Add(WidgetAnimation);

			for (int32 Index = 0; Index < WidgetAnimation->MovieScene->GetPossessableCount(); ++Index) {
				FMovieScenePossessable& Possessable = WidgetAnimation->MovieScene->GetPossessable(Index);

				TArray<UWidget*> Widgets;
				WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);

				for (UWidget* Widget : Widgets) {
					if (Widget->GetName() == Possessable.GetName()) {
#if ENGINE_UE5
						Possessable.SetPossessedObjectClass(Widget->GetClass());
#endif
					}
				}
			}
			
			for (const FMovieSceneBinding& Binding : WidgetAnimation->MovieScene->GetBindings()) {
				for (UMovieSceneTrack* Track : Binding.GetTracks()) {
					Track->Modify();
					Track->MarkAsChanged();

					if (UMovieSceneWidgetMaterialTrack* MaterialTrack = Cast<UMovieSceneWidgetMaterialTrack>(Track)) {
						MaterialTrack->SetDisplayName(FText::FromString(MaterialTrack->GetBrushPropertyNamePath()[0].ToString()));
					}
				}
			}
		}
	});
}