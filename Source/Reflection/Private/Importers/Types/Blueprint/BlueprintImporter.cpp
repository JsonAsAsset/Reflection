/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BlueprintImporter.h"

#include "KismetCompilerModule.h"
#include "MovieScene.h"
#include "WidgetBlueprint.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/MovieSceneWidgetMaterialTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

#include "Engine/SCS_Node.h"
#include "Importers/Types/Blueprint/BlueprintUtilities.h"
#include "Importers/Types/Blueprint/BlueprintVariables.h"
#include "Importers/Types/Blueprint/BlueprintGraphs.h"
#include "K2Node_Timeline.h"
#include "Engine/TimelineTemplate.h"
#include "Importers/Types/Blueprint/BlueprintCookedMetaData.h"
#include "Importers/Types/Blueprint/BytecodeGraph.h"

UObject* IBlueprintImporter::CreateAsset(UObject* CreatedAsset) {
	UClass* Class = GetAssetClass();
	
	if (!Class) {
		FImportIssues::Report(
			EImportIssue::MissingClass,
			TEXT("Couldn't resolve the parent class"),
			TEXT("The blueprint's parent class could not be found or loaded. Verify that the class is defined and available when reflecting.")
		);

		return nullptr;
	}
	
	/* Find the blueprint class and generated class */
	UClass* BlueprintClass = nullptr, *GeneratedClass = nullptr;
	
	FModuleManager::LoadModuleChecked<IKismetCompilerInterface>
		("KismetCompiler")
			.GetBlueprintTypesForClass(
				Class,
				BlueprintClass,
				GeneratedClass
			);

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

	/* A blueprint with no class default object export has nothing to deserialize defaults from,
	 * and writing to what the lookup handed back would land on the shared empty export */
	if (ClassDefaultObjectExport->IsJsonInvalid()) return false;

	ClassDefaultObjectExport->Object = GeneratedClass;

	/* The variables have to exist before their defaults can land anywhere. A recreated blueprint
	 * only has what its parent class gave it, so any property the blueprint declared itself is
	 * missing, and deserializing the class default object over it would drop those values on the
	 * floor without complaining. */
	if (ConstructVariables() > 0) {
		/* Adding a variable only touches the blueprint, the generated class grows the property
		 * when it recompiles, and the default object below is the one that comes out of that */
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

		GeneratedClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
		if (!GeneratedClass) return false;

		ClassDefaultObjectExport->Object = GeneratedClass;
	}

	GetObjectSerializer()->DeserializeObjectProperties(ClassDefaultObjectExport->GetProperties(), GeneratedClass->GetDefaultObject());

	/* Experimental (for now) spawning */
	GetObjectSerializer()->bUseExperimentalSpawning = true;

	ConstructScript();
	ConstructWidgetTree();

	/* Before the graphs, since a graph that starts one reads the pins it grew */
	if (const int32 Timelines = ConstructTimelines(); Timelines > 0) {
		UE_LOG(LogReflection, Display, TEXT("%d timeline(s) rebuilt"), Timelines);
	}

	const int32 Laid = ConstructGraphs();

	/* What survived, counted where it matters rather than where it was made */
	if (const UEdGraph* Events = FBlueprintGraphs::Events(Blueprint)) {
		int32 Standing = 0;

		for (UEdGraphNode* Node : Events->Nodes) {
			if (Node != nullptr && Node->IsA<UK2Node_Timeline>()) Standing++;
		}

		UE_LOG(LogReflection, Display, TEXT("%d timeline node(s) still in the event graph after laying it out"), Standing);
	}

	/* Said once both the variables and the graphs it names exist, since it is said over them */
	if (const int32 Said = FBlueprintCookedMetaData::Apply(Blueprint, GetContainer()); Said > 0) {
		UE_LOG(LogReflection, Display, TEXT("%d thing(s) the editor knew were kept through the cook and said again"), Said);
	}

	/* A graph is only worth compiling once there is something in it */
	if (Laid > 0) {
 		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

		/* What the compile made of it, which is the thing a rebuilt graph is judged by */
		if (const UBlueprintGeneratedClass* Compiled = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass)) {
			for (TFieldIterator<UFunction> It(Compiled, EFieldIteratorFlags::ExcludeSuper); It; ++It) {
				UE_LOG(LogReflection, Display, TEXT("compiled \"%s\" to %d byte(s) of script"), *It->GetName(), It->Script.Num());
			}
		}
	}

	return OnAssetCreation(Blueprint);
}

int32 IBlueprintImporter::ConstructVariables() {
	/* What the construction script gives the blueprint is a component, not a variable, even though
	 * the class declares a property for it the same way it declares any other */
	const TSet<FString> Components = FBlueprintVariables::GetComponentVariables(GetContainer());

	TArray<TSharedPtr<FJsonValue>> Declared;

	/* A blueprint that declared nothing of its own declares nothing here either */
	for (const TSharedPtr<FJsonValue>& Value : FBlueprintVariables::GetDeclared(GetAssetExport(), GetContainer())) {
		const TSharedPtr<FJsonObject> Property = Value.IsValid() ? Value->AsObject() : nullptr;

		FString Name;

		if (Property.IsValid() && Property->TryGetStringField(TEXT("Name"), Name) && Components.Contains(Name)) continue;

		Declared.Add(Value);
	}

	return FBlueprintVariables::Construct(Blueprint, Declared);
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

		/* What the class was keeping for the old script goes with it.
		 *
		 * A template that has been moved out is still one the class says it keeps, and the engine
		 * refuses a class outright when it says it keeps something kept somewhere else. Refused, it
		 * never picks up the components the new script gives it, every node that reads one is left
		 * pointing at nothing, and the run falls apart: which is why importing an asset a second
		 * time comes back worse than importing it once. */
		const auto Forget = [GeneratedClass](auto& Kept) {
			Kept.RemoveAll([GeneratedClass](const auto& Component) {
				return Component == nullptr || Component->GetOuter() != GeneratedClass;
			});
		};

		Forget(Blueprint->ComponentTemplates);
		Forget(GeneratedClass->ComponentTemplates);
	}

	FUObjectExport* Export = GetContainer()->GetExportByObjectPath(GetAssetDataAsValue().GetObject("SimpleConstructionScript"));

	/* Spawn the new Construction Script */
	USimpleConstructionScript* SimpleConstructionScript =
		Cast<USimpleConstructionScript>(
			GetObjectSerializer()->SpawnExport(Export)
		);

	UE_LOG(LogReflection, Display, TEXT("construction script: export %s, spawned %s, %d node(s)"),
		Export != nullptr && Export->IsJsonValid() ? TEXT("found") : TEXT("MISSING"),
		SimpleConstructionScript != nullptr ? TEXT("yes") : TEXT("NO"),
		SimpleConstructionScript != nullptr ? SimpleConstructionScript->GetAllNodes().Num() : -1);

	if (SimpleConstructionScript == nullptr) return;

	/* Update SimpleConstructionScript on the Blueprint */
	Blueprint->SimpleConstructionScript = SimpleConstructionScript;
	GeneratedClass->SimpleConstructionScript = SimpleConstructionScript;

	/* Engine Ensures */
	SimpleConstructionScript->FixupRootNodeParentReferences();
	SimpleConstructionScript->ValidateSceneRootNodes();

	/* A node with nothing to copy is a component that never comes to exist.
	 *
	 * The template is an export of its own, and spawning it can come back with nothing where an
	 * earlier import left an object still holding the name. Once that happens the class has no such
	 * component, every node that reads one is left pointing at nothing, and the run falls apart:
	 * which is why the same asset imported twice comes back right every other time.
	 *
	 * So a node left without one is given it outright: whatever holds the name is moved aside, and
	 * the template is made where the class keeps it. */
	for (USCS_Node* Node : SimpleConstructionScript->GetAllNodes()) {
		if (Node == nullptr || Node->ComponentTemplate != nullptr) continue;

		const FString Wanted = Node->GetVariableName().ToString() + TEXT("_GEN_VARIABLE");

		FUObjectExport* Made = GetContainer()->Find(FName(*Wanted));

		if (Made == nullptr || !Made->IsJsonValid()) continue;

		UClass* Kind = Made->GetClass();

		if (Kind == nullptr || !Kind->IsChildOf(UActorComponent::StaticClass())) continue;

		if (UObject* Holding = StaticFindObject(nullptr, GeneratedClass, *Wanted)) {
			MoveToTransientPackageAndRename(Holding);
		}

		UActorComponent* Template = NewObject<UActorComponent>(GeneratedClass, Kind, *Wanted, RF_ArchetypeObject | RF_Public | RF_Transactional);

		if (Template == nullptr) continue;

		Made->Object = Template;

		GetObjectSerializer()->DeserializeObjectProperties(Made->GetProperties(), Template);

		Node->ComponentTemplate = Template;

		UE_LOG(LogReflection, Display, TEXT("\"%s\" had no template of its own, so one was made where the class keeps it"), *Wanted);
	}

	for (const USCS_Node* Node : SimpleConstructionScript->GetAllNodes()) {
		UE_LOG(LogReflection, Display, TEXT("  scs \"%s\" template %s kept by %s (class keeps %d, blueprint keeps %d)"),
			*Node->GetVariableName().ToString(),
			*GetNameSafe(Node->ComponentTemplate),
			Node->ComponentTemplate != nullptr ? *GetNameSafe(Node->ComponentTemplate->GetOuter()) : TEXT("-"),
			GeneratedClass->ComponentTemplates.Num(),
			Blueprint->ComponentTemplates.Num());
	}
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

	/* Same as above: the empty export is shared, so a miss here must not be written to */
	if (ClassDefaultObjectExport->IsJsonValid()) {
		ClassDefaultObjectExport->Object = WidgetBlueprint;
	}

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
			
			const UMovieScene* MovieScene = WidgetAnimation->MovieScene;
			for (const FMovieSceneBinding& Binding : MovieScene->GetBindings()) {
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
int32 IBlueprintImporter::ConstructGraphs() {
	if (Blueprint == nullptr) return 0;

	int32 Placed = 0;

	/* Anything done to the blueprint that is not a node being laid out. A graph made, an event
	 * written or taken back out is worth a compile on its own, since none of it reaches the class
	 * the blueprint generates until one happens. */
	int32 Changed = 0;

	TArray<TSharedPtr<FBytecodeGraph>> Builders;

	/* The events are laid out into the one graph they were all written in, so it is built last and
	 * entered once per event rather than once */
	/* Whether anything was made that the class does not know about yet */
	bool bSignatures = false;

	TSharedPtr<FBytecodeGraph> Ubergraph;
	TArray<TPair<FString, FBlueprintGraphs::FWritten>> Events;

	/* Where each timeline picks its run back up, against the node and the way out of it */
	TArray<TTuple<int32, TWeakObjectPtr<UK2Node>, FName>> Resumed;
	TMap<FString, TArray<TSharedPtr<FJsonValue>>> Signatures;

	for (FUObjectExport* Export : GetContainer()->Exports) {
		if (Export == nullptr || !Export->IsJsonValid()) continue;
		if (Export->GetType() != TEXT("Function")) continue;
		if (!Export->Has(TEXT("ScriptBytecode"))) continue;

		const FString Name = Export->GetName().ToString();

		/* What a dispatcher hands over is declared as a function of its own, and it is not one:
		 * nobody wrote it, nobody calls it, and it is laid out as the dispatcher below instead. */
		if (Name.EndsWith(TEXT("__DelegateSignature"))) continue;

		const FUObjectJsonValueExport Function(Export->JsonObject);

		/* What the function keeps of its own, and what it takes: an older asset writes both out as
		 * exports of their own and names them from the function's Children rather than writing
		 * them into it */
		const TArray<TSharedPtr<FJsonValue>> Declared = FBlueprintVariables::GetDeclared(Export->JsonObject, GetContainer());

		TArray<FUObjectJsonValueExport> Locals;

		for (const TSharedPtr<FJsonValue>& Local : Declared) {
			Locals.Add(FUObjectJsonValueExport(Local));
		}

		/* Not everything the class carries was written as a function, and which it was is read from
		 * the function rather than guessed from its name */
		const FBlueprintGraphs::FWritten Written = FBlueprintGraphs::Reads(Function);

		/* What a timeline calls as it plays reads as an event, and it is not one: nobody wrote it,
		 * and what it runs was drawn coming out of the timeline. So it is taken as a way into the
		 * ubergraph like an event is, entered through the timeline's own pin. */
		if (const TPair<TWeakObjectPtr<UK2Node>, FName>* Resuming = Resumes.Find(FName(*Name))) {
			if (Written.Kind == FBlueprintGraphs::EWritten::Event && Resuming->Key.IsValid()) {
				Resumed.Emplace(Written.EntryPoint, Resuming->Key, Resuming->Value);
			}

			continue;
		}

		if (Written.Kind == FBlueprintGraphs::EWritten::Event) {
			Signatures.Add(Name, Declared);
			Events.Add(TPair<FString, FBlueprintGraphs::FWritten>(Name, Written));

			continue;
		}

		/* The graph it was written in, which a recreated blueprint has only where its parent gave
		 * it one. Everything else the blueprint added has to be made again before it can be filled. */
		UEdGraph* Graph = nullptr;

		if (Written.Kind == FBlueprintGraphs::EWritten::Ubergraph) {
			Graph = FBlueprintGraphs::Events(Blueprint);
		} else {
			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);

			for (UEdGraph* Candidate : Graphs) {
				if (Candidate != nullptr && Candidate->GetName() == Name) {
					Graph = Candidate;

					break;
				}
			}

			if (Graph == nullptr) {
				Graph = FBlueprintGraphs::Make(Blueprint, Name, Function, Declared);

				bSignatures = bSignatures || Graph != nullptr;

				Changed++;
			}
		}

		if (Graph == nullptr) {
			FImportIssues::Report(
				EImportIssue::MissingClass,
				TEXT("No graph for a function the class carries"),
				FString::Printf(TEXT("'%s' was cooked with bytecode, and no graph could be made to lay it back out in."), *Name)
			);

			continue;
		}

		TSharedPtr<FBytecodeGraph> Made = MakeShared<FBytecodeGraph>(
			Graph,
			Export->GetArray(TEXT("ScriptBytecode")),
			Locals,
			GetContainer()
		);

		/* Whatever the timelines hand out, said to every graph that might read it */
		for (const TPair<FString, TPair<TWeakObjectPtr<UK2Node>, FName>>& Handout : Handouts) {
			if (Handout.Value.Key.IsValid()) {
				Made->HandOverTrack(Handout.Key, Handout.Value.Key.Get(), Handout.Value.Value);
			}
		}

		if (Written.Kind == FBlueprintGraphs::EWritten::Ubergraph) {
			Ubergraph = Made;
		}

		Builders.Add(Made);
	}

	/* The delegates the blueprint declares.
	 *
	 * A dispatcher is neither a variable nor a function: it is a delegate other things bind to, and
	 * the class carries it among its properties while what it hands over is declared as a function
	 * named after it. Without it, everything that binds to one or broadcasts one is pointing at a
	 * delegate the class does not have. */
	for (const TSharedPtr<FJsonValue>& Value : FBlueprintVariables::GetDeclared(GetAssetExport(), GetContainer())) {
		const TSharedPtr<FJsonObject> Property = Value.IsValid() ? Value->AsObject() : nullptr;

		if (!Property.IsValid()) continue;

		FString Kind;

		if (!Property->TryGetStringField(TEXT("Type"), Kind)) continue;
		if (!Kind.StartsWith(TEXT("Multicast")) || !Kind.EndsWith(TEXT("DelegateProperty"))) continue;

		FString Named;

		if (!Property->TryGetStringField(TEXT("Name"), Named) || Named.IsEmpty()) continue;

		FUObjectExport* Signature = GetContainer()->Find(FName(*(Named + TEXT("__DelegateSignature"))));

		const TArray<TSharedPtr<FJsonValue>> Hands = Signature != nullptr && Signature->IsJsonValid()
			? FBlueprintVariables::GetDeclared(Signature->JsonObject, GetContainer())
			: TArray<TSharedPtr<FJsonValue>>();

		const bool bMade = FBlueprintGraphs::MakeDispatcher(Blueprint, Named, Hands) != nullptr;

		UE_LOG(LogReflection, Display, TEXT("dispatcher \"%s\": signature %s, %d handed over, made %s"),
			*Named,
			Signature != nullptr && Signature->IsJsonValid() ? TEXT("found") : TEXT("MISSING"),
			Hands.Num(),
			bMade ? TEXT("yes") : TEXT("NO"));

		if (bMade) bSignatures = true;
	}

	/* Every event is a way into the ubergraph, at the address its own function passes along */
	if (Ubergraph.IsValid()) {
		UEdGraph* EventGraph = FBlueprintGraphs::Events(Blueprint);

		TSet<FString> Written;

		for (const TPair<FString, FBlueprintGraphs::FWritten>& Event : Events) {
			UK2Node* Node = FBlueprintGraphs::MakeEvent(Blueprint, EventGraph, Event.Key, Signatures.FindRef(Event.Key));

			bSignatures = bSignatures || Node != nullptr;

			if (Node == nullptr) continue;

			Written.Add(Event.Key);

			Changed++;

			Ubergraph->EnterAt(Event.Value.EntryPoint, Node);

			for (const TPair<FString, FString>& Given : Event.Value.Frame) {
				Ubergraph->HandOver(Given.Key, Given.Value);
			}
		}

		for (const TTuple<int32, TWeakObjectPtr<UK2Node>, FName>& Resuming : Resumed) {
			if (!Resuming.Get<1>().IsValid()) continue;

			Ubergraph->EnterAt(Resuming.Get<0>(), Resuming.Get<1>().Get(), Resuming.Get<2>());

			Changed++;
		}

		/* What the editor puts in a new blueprint to be helpful: a node for each of the events its
		 * parent offers most often, sat there waiting to be used. An event node compiles to an
		 * event whether or not anything runs from it, so leaving one the game's class never carried
		 * would put a function in the class that was never there. */
		Changed += FBlueprintGraphs::RemoveUnwrittenEvents(EventGraph, Written);
	} else if (Events.Num() > 0) {
		FImportIssues::Report(
			EImportIssue::MissingClass,
			TEXT("Events with nowhere to lay them out"),
			FString::Printf(TEXT("%d event(s) were cooked with bytecode, and the class carries no ubergraph holding their bodies."), Events.Num())
		);
	}

	/* Everything the functions keep of their own is declared first and settled with a compile, since
	 * declaring one lays the graph out again and would undo anything already placed */
	/* Emptied before anything is settled, so what is compiled below is these graphs as they are
	 * about to be written and not as the run before left them */
	for (const TSharedPtr<FBytecodeGraph>& Builder : Builders) {
		Builder->Clear();
	}

	int32 Declared = 0;

	for (const TSharedPtr<FBytecodeGraph>& Builder : Builders) {
		Declared += Builder->DeclareLocals();
	}

	/* Settled before anything is laid out.
	 *
	 * A call to one of these is laid out from the function's signature as the class carries it, and
	 * what a graph says its function takes and gives back only reaches the class when the blueprint
	 * is compiled. Laid out before that, every call comes back without the pins for whatever the
	 * function was given or hands back, and the reader has to refresh the node by hand to see them. */
	/* Always, and not only where something new was made.
	 *
	 * Every node is laid out from what the class carries: a call from its signature, a variable read
	 * from the property behind it. The construction script hands the class its components, the
	 * graphs hand it their signatures, and neither reaches it until it is compiled.
	 *
	 * Made conditional, a second import of the same asset finds its graphs already there, compiles
	 * nothing, and lays every node out against a class that has not caught up: the same asset comes
	 * back differently depending on what the run before it left behind. */
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	}

	for (const TSharedPtr<FBytecodeGraph>& Builder : Builders) {
		Placed += Builder->Build();
	}

	return Placed + Changed;
}

int32 IBlueprintImporter::ConstructTimelines() {
	if (Blueprint == nullptr) return 0;

	UEdGraph* Events = FBlueprintGraphs::Events(Blueprint);

	int32 Made = 0;

	for (FUObjectExport* Export : GetContainer()->Exports) {
		if (Export == nullptr || !Export->IsJsonValid()) continue;
		if (Export->GetType() != TEXT("TimelineTemplate")) continue;

		const FUObjectJsonValueExport Held(Export->GetProperties());

		/* What the node is called. The template beside it is named after the same thing with a
		 * suffix the engine puts on, so the name to build from is the one written inside. */
		const FString Called = Held.Has(TEXT("VariableName")) ? Held.GetString(TEXT("VariableName")) : FString();

		if (Called.IsEmpty()) continue;

		UTimelineTemplate* Template = Blueprint->FindTimelineTemplateByVariableName(FName(*Called));

		if (Template == nullptr) {
			Template = FBlueprintEditorUtils::AddNewTimeline(Blueprint, FName(*Called));
		}

		if (Template == nullptr) continue;

		Export->Object = Template;

		/* The curves first, and made here rather than fetched.
		 *
		 * A track names its curve, and the curve is kept inside this same asset rather than being
		 * an asset of its own. Left to be resolved as an ordinary reference it is looked for in the
		 * project, not found, and asked of the cloud by the path it sits at, which is the path of
		 * the blueprint being imported: the import starts again from the top, reaches this same
		 * line, and does it all over until the stack gives out. Made first, the reference is
		 * answered from memory and never leaves. */
		for (FUObjectExport* Curve : GetContainer()->Exports) {
			if (Curve == nullptr || !Curve->IsJsonValid() || Curve->Object != nullptr) continue;
			if (!Curve->GetType().ToString().StartsWith(TEXT("Curve"))) continue;

			GetObjectSerializer()->SpawnExport(Curve);
		}

		/* Said whole rather than field by field. How long it runs, whether it loops, every track
		 * and the curve behind it, and the names of the two functions it calls are all written out
		 * beside it, and reading them one by one here would only be a shorter list than the one the
		 * engine already knows how to fill in. */
		GetObjectSerializer()->DeserializeObjectProperties(Export->GetProperties(), Template);

		/* The order the tracks are shown in, which the cook does not keep.
		 *
		 * A timeline node grows one pin per track, and it reads them off this order rather than off
		 * the tracks themselves. The order is editor-only data, so a cooked template arrives with
		 * every track intact and nothing saying how to show them: the node comes back with its ways
		 * in and out and not one of its tracks. Put back in the order the tracks are kept in, which
		 * is the order they were added. */
		if (Template->GetNumDisplayTracks() == 0) {
			for (int32 Index = 0; Index < Template->EventTracks.Num(); ++Index) {
				Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_Event, Index));
			}

			for (int32 Index = 0; Index < Template->FloatTracks.Num(); ++Index) {
				Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_FloatInterp, Index));
			}

			for (int32 Index = 0; Index < Template->VectorTracks.Num(); ++Index) {
				Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_VectorInterp, Index));
			}

			for (int32 Index = 0; Index < Template->LinearColorTracks.Num(); ++Index) {
				Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_LinearColorInterp, Index));
			}
		}

		/* And the node that starts it, which is what somebody actually drew */
		if (Events != nullptr) {
			UK2Node_Timeline* Node = nullptr;

			for (UEdGraphNode* Sat : Events->Nodes) {
				if (UK2Node_Timeline* Already = Cast<UK2Node_Timeline>(Sat); Already != nullptr && Already->TimelineName == FName(*Called)) {
					Node = Already;

					break;
				}
			}

			if (Node == nullptr) {
				Node = NewObject<UK2Node_Timeline>(Events);

				Events->AddNode(Node, false, false);

				Node->CreateNewGuid();
				Node->PostPlacedNewNode();
			}

			Node->TimelineName = FName(*Called);
			Node->bAutoPlay = Template->bAutoPlay;
			Node->bLoop = Template->bLoop;
			Node->bReplicated = Template->bReplicated;
			Node->bIgnoreTimeDilation = Template->bIgnoreTimeDilation;
			Node->TimelineGuid = Template->TimelineGuid;

			/* Laid out from the template, since its pins are one per track */
			Node->ReconstructNode();

			/* And said, so the script's reads of each track land on the pin it was drawn as rather
			 * than on the property the timeline keeps it in */
			for (const FTTFloatTrack& Track : Template->FloatTracks) {
				Handouts.Add(Track.GetPropertyName().ToString(), TPair<TWeakObjectPtr<UK2Node>, FName>(Node, Track.GetTrackName()));
			}

			for (const FTTVectorTrack& Track : Template->VectorTracks) {
				Handouts.Add(Track.GetPropertyName().ToString(), TPair<TWeakObjectPtr<UK2Node>, FName>(Node, Track.GetTrackName()));
			}

			for (const FTTLinearColorTrack& Track : Template->LinearColorTracks) {
				Handouts.Add(Track.GetPropertyName().ToString(), TPair<TWeakObjectPtr<UK2Node>, FName>(Node, Track.GetTrackName()));
			}

			Handouts.Add(Template->GetDirectionPropertyName().ToString(), TPair<TWeakObjectPtr<UK2Node>, FName>(Node, TEXT("Direction")));

			/* And what it calls as it plays. A timeline runs those two the way an event is run,
			 * so each is a way into the ubergraph, entered through the pin it stands for rather
			 * than through an event of its own. Both names are the template's own. */
			if (const UEdGraphPin* Playing = Node->GetUpdatePin()) {
				Resumes.Add(Template->GetUpdateFunctionName(), TPair<TWeakObjectPtr<UK2Node>, FName>(Node, Playing->PinName));
			}

			if (const UEdGraphPin* Done = Node->GetFinishedPin()) {
				Resumes.Add(Template->GetFinishedFunctionName(), TPair<TWeakObjectPtr<UK2Node>, FName>(Node, Done->PinName));
			}
		}

		Made++;

		UE_LOG(LogReflection, Display, TEXT("timeline \"%s\": %d float track(s), %g long"), *Called, Template->FloatTracks.Num(), Template->TimelineLength);
	}

	if (Made > 0) {
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	return Made;
}
