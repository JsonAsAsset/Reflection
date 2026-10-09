/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/World/LevelRead.h"

#include "Containers/Export.h"
#include "Containers/ExportContainer.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Importers/Constructor/Asset.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Importers/Constructor/Types.h"
#include "Modules/Toolbar/Tools/ImportFromPath.h"
#include "Settings/ReflectionSettings.h"
#include "Settings/SettingsAccess.h"
#include "Utilities/JsonHelpers.h"

#include "Engine/EngineUtilities.h"
#include "Engine/Log.h"
#include "Engine/Package.h"
#include "Utilities/AssetPaths.h"
#include "Importers/Constructor/TypesHelper.h"
#include "Modules/Toolbar/Tools/ImportFromPath.h"
#include "ThumbnailRendering/WorldThumbnailInfo.h"

#include "Editor.h"
#include "Engine/Brush.h"
#include "Engine/Level.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "Engine/LODActor.h"
#include "Engine/HLODProxy.h"
#include "HLOD/HLODProxyDesc.h"
#include "HierarchicalLOD.h"

/* [4.26] 	4.26 doesn't find the Importer header on it's own.
 * 			Let's help it out. */
#if UE4_26
#include "Importers/Constructor/Importer.h"
#endif

/* An actor sits directly inside the level; everything else inside a package sits inside something */
static const FName GActorOuter = TEXT("PersistentLevel");

namespace {
	/* What the export says it is of: a bare name, or a path where a blueprint generated it */
	FString NamedClass(const FUObjectExport* Export) {
		FString Named = Export->GetString(TEXT("Class"));

		if (Named.Contains(TEXT("'"))) {
			Named.Split(TEXT("'"), nullptr, &Named);
			Named.Split(TEXT("'"), &Named, nullptr);
		}

		return Named;
	}

	/* Whether the class behind an export is one a blueprint generated */
	bool IsBlueprintMade(const FUObjectExport* Export) {
		return NamedClass(Export).Contains(TEXT("/"));
	}

	/* What decides the class, in the order it is read for: a placed actor names its template */
	FString DecidesClass(const FUObjectExport* Export) {
		if (Export->Has(TEXT("Template"))) {
			const FString Named = Export->GetObject(TEXT("Template")).GetString(TEXT("ObjectName"));

			if (!Named.IsEmpty()) {
				return Named;
			}
		}

		const FString Named = Export->GetString(TEXT("Class"));

		return Named.IsEmpty() ? Export->GetType().ToString() : Named;
	}

	/* What a reference names, down to the leaf */
	FName NamedExport(const FUObjectJsonValueExport& Reference) {
		const FString Named = Reference.GetString(TEXT("ObjectName"));

		return Named.IsEmpty() ? FName() : FName(*GetObjectNameFromOuter(Named));
	}

	/* The asset a generated class belongs to, which is what has to come across */
	FString AssetOfClass(const FUObjectExport* Export) {
		FString Asset = NamedClass(Export);

		Asset.Split(TEXT("."), &Asset, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);

		return Asset;
	}
}

bool FLevelRead::Handles(const TArray<TSharedPtr<FJsonValue>>& Exports) {
	for (const TSharedPtr<FJsonValue>& Value : Exports) {
		const TSharedPtr<FJsonObject> Export = Value.IsValid() ? Value->AsObject() : nullptr;

		if (!Export.IsValid()) continue;

		if (FString Type; Export->TryGetStringField(TEXT("Type"), Type) && Type == TEXT("World")) {
			return true;
		}
	}

	return false;
}

bool FLevelRead::FromCloud(const TArray<TSharedPtr<FJsonValue>>& Exports, const FString& Path, FRLevelReadResult* OutResult) {
	/* Switched off rather than absent, said the way every other experimental type says it */
	if (!ImportTypes::Allowed(TEXT("World"))) {
		FImportIssues::ReportFor(FPaths::GetBaseFilename(Path), Path, TEXT("World"), EImportIssue::Setting,
			TEXT("This type is behind Enable Experiments"),
			TEXT("Reading a level is experimental, and Reflection reads one once Enable Experiments is turned on in its settings."));

		return false;
	}

	FLevelRead Reading;

	const FRLevelReadResult Result = Reading.Read(Exports, Path);

	if (OutResult != nullptr) {
		*OutResult = Result;
	}

	return true;
}

bool FLevelRead::Allows(const FUObjectExport* Export, bool& bOutMayFetch) const {
	const ERLevelActors Wanted = GetSettings()->Level.Actors;

	bOutMayFetch = false;

	switch (Wanted) {
		case ERLevelActors::Everything:
			bOutMayFetch = true;

			return true;

		/* Nothing is fetched to place an actor, so a class that has to be is a class we haven't got */
		case ERLevelActors::NoBlueprints:
		case ERLevelActors::StaticMeshesOnly:
			return !IsBlueprintMade(Export);

		/* Told from the export rather than from the class it resolves to, so the other few thousand
		 * are turned down before anything is looked up or fetched for them */
		case ERLevelActors::HLODsOnly:
			return Export->GetType() == TEXT("LODActor");

		default:
			return false;
	}
}

namespace {
	/* Whether an actor of this can be made: an unfinished class asserts inside the engine rather than handing back null */
	bool Spawnable(const UClass* Class) {
		if (!ClassIsFormed(Class)) return false;

		/* Neither is anything a level would place */
		return !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated);
	}
}

UClass* FLevelRead::ClassOf(FUObjectExport* Export, const bool bMayFetch, TMap<FString, UClass*>& Known, FRLevelReadResult& Result) const {
	const FString Named = DecidesClass(Export);

	/* Looked up once per class, or a prop placed a hundred times is a hundred round trips */
	if (UClass** Cached = Known.Find(Named)) {
		return *Cached;
	}

	/* Asked of the export rather than looked up by name, since only it knows what it is of */
	if (UClass* Standing = Export->GetClass()) {
		Known.Add(Named, Standing);

		return Standing;
	}

	if (!bMayFetch || !IsBlueprintMade(Export)) {
		Known.Add(Named, nullptr);

		return nullptr;
	}

	/* The class comes off the export's header rather than a property, so nothing fetches it. Asked for as the asset, not the class. */
	const FString Asset = AssetOfClass(Export);

	if (Asset.IsEmpty()) {
		Known.Add(Named, nullptr);

		return nullptr;
	}

	if (!TToolImportFromPath::Import(Asset)) {
		FImportIssues::Report(EImportIssue::MissingAsset, TEXT("Couldn't reflect ") + Asset, TEXT("Named as the class of an actor in this level"));
	}

	UClass* Fetched = Export->GetClass();

	Known.Add(Named, Fetched);

	return Fetched;
}

UWorld* FLevelRead::LevelFor(UPackage* Package) const {
	/* Already here, so this reads over it: a second world in the package collides on its world settings */
	if (UWorld* Standing = UWorld::FindWorldInPackage(Package)) {
		int32 Removed = 0;

		/* Off the level, not an actor iterator: one straight out of a package has been told of no levels and comes back empty */
		TArray<AActor*> Held;

		if (const ULevel* Level = Standing->PersistentLevel) {
			Held = Level->Actors;
		}

		for (AActor* Actor : Held) {
			/* The two the level is made with rather than filled with */
			if (Actor == nullptr || Actor->IsA<AWorldSettings>() || Actor == Standing->GetDefaultBrush()) continue;

			/* The editor's own destroy, which unpicks what the editor keeps about an actor */
			if (Standing->EditorDestroyActor(Actor, true)) Removed++;
		}

		UE_LOG(LogReflection, Display, TEXT("reading over the level already at \"%s\", %d actor(s) cleared"), *Package->GetName(), Removed);

		return Standing;
	}

	/* What the editor makes a level asset with: no navigation and no AI, since this one is
	 * written out rather than played or edited */
	const UWorld::InitializationValues Values = UWorld::InitializationValues()
		.ShouldSimulatePhysics(false)
		.EnableTraceCollision(true)
		.CreateNavigation(false)
		.CreateAISystem(false);


	/* [4.26]	This overload of CreateWorld does not exist under 4.26. */
	#if !UE4_26
	/* Named for the asset, and not rooted: a rooted world is still standing when its own level is opened */
	UWorld* Made = UWorld::CreateWorld(EWorldType::Inactive, false, FName(*FPackageName::GetShortName(Package)),
		Package, /* bAddToRoot */ false, ERHIFeatureLevel::Num, &Values);
	#else
	UWorld* Made = UWorld::CreateWorld(EWorldType::Inactive, false, FName(*FPackageName::GetShortName(Package)),
		Package, /* bAddToRoot */ false, ERHIFeatureLevel::Num);
	#endif
	

	if (Made != nullptr) {
		/* Standalone, or nothing points at it: the save drops it and a collection takes it */
		Made->SetFlags(RF_Public | RF_Standalone);

		/* What a level is made with, which the editor expects every level to have */
		if (GEditor != nullptr) GEditor->InitBuilderBrush(Made);

		/* What the content browser draws it as, which every level made as an asset carries */
		Made->ThumbnailInfo = NewObject<UWorldThumbnailInfo>(Made, NAME_None, RF_Transactional);
	}

	return Made;
}

void FLevelRead::ReadWorldSettings(UWorld* World, FUObjectExport* Export) const {
	AWorldSettings* Settings = World != nullptr ? World->GetWorldSettings() : nullptr;

	if (Settings == nullptr) {
		FImportIssues::Report(EImportIssue::Failed, TEXT("The level has no world settings to write onto"));

		return;
	}

	/* Written onto the ones the level already has: spawned, it would be a second nobody uses */
	GetObjectSerializer()->Parent = Settings;
	GetObjectSerializer()->DeserializeObjectProperties(Export->GetProperties(), Settings);

	Settings->Modify();
	Settings->PostEditChange();

	/* Said plainly, because what the level is like to play in changed and nothing on screen shows it */
	UE_LOG(LogReflection, Display, TEXT("world settings read onto \"%s\""), *Settings->GetName());
}

void FLevelRead::SubLevelsNamed(const TArray<TSharedPtr<FJsonValue>>& Exports, FUObjectExport* World, TArray<FString>& OutNamed) {
	/* Said once however many name it: a POI stands several foundations on one building */
	const auto Named = [&OutNamed](FString Path) {
		if (Path.IsEmpty()) return;

		/* The package rather than the asset in it, which is what a read is asked for */
		FString Package;
		Path = Path.Split(TEXT("."), &Package, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd) ? Package : Path;

		if (!Path.IsEmpty()) OutNamed.AddUnique(Path);
	};

	/* What a foundation stands for, which is where a POI keeps its buildings */
	for (const TSharedPtr<FJsonValue>& Value : Exports) {
		const TSharedPtr<FJsonObject> Export = Value.IsValid() ? Value->AsObject() : nullptr;

		if (!Export.IsValid()) continue;

		const TSharedPtr<FJsonObject>* Properties;

		if (!Export->TryGetObjectField(TEXT("Properties"), Properties) || !Properties->IsValid()) continue;

		const TArray<TSharedPtr<FJsonValue>>* Worlds;

		if (!(*Properties)->TryGetArrayField(TEXT("AdditionalWorlds"), Worlds)) continue;

		for (const TSharedPtr<FJsonValue>& Each : *Worlds) {
			const TSharedPtr<FJsonObject> Entry = Each.IsValid() ? Each->AsObject() : nullptr;

			if (!Entry.IsValid()) continue;

			FString Asset;

			if (Entry->TryGetStringField(TEXT("AssetPathName"), Asset)) Named(Asset);
		}
	}

	/* And what it streams, for a level that holds its sub levels the ordinary way */
	if (World == nullptr || !World->Has(TEXT("StreamingLevels"))) {
		return;
	}

	for (const FUObjectJsonValueExport& Each : World->GetArray(TEXT("StreamingLevels"))) {
		FString Package = Each.GetString(TEXT("PackageName"));

		/* Named on the streaming object where it carries it, and by where it points when it does not */
		if (Package.IsEmpty()) Package = Each.GetString(TEXT("ObjectPath"));

		Named(Package);
	}
}

void FLevelRead::ReadHLODSetup(UWorld* World, const TArray<ALODActor*>& Actors) const {
	AWorldSettings* Settings = World != nullptr ? World->GetWorldSettings() : nullptr;

	/* [4.26] 	Under the 4.26.2 engine, we don't have access to IsEmpty on a TArray.
	 *			Getting Max == 0 *should* return the same functionality. */
	#if !UE4_26
	if (Settings == nullptr || Actors.IsEmpty()) return;
	#else
	if (Settings == nullptr || Actors.Max() == 0) return;
	#endif

	/* As many levels as the actors say, rather than guessed: each carries the one it belongs to */
	int32 Levels = 0;

	for (const ALODActor* Actor : Actors) {
		if (Actor != nullptr) Levels = FMath::Max(Levels, Actor->LODLevel);
	}

	if (Levels <= 0) return;

	TArray<FHierarchicalSimplification>& Setup = Settings->GetHierarchicalLODSetup();

	/* Left alone where the level already says something, since what is put here is only a count */
	if (Setup.Num() >= Levels) return;

	Setup.SetNum(Levels);

	/* How far off each draws, which is the one thing about the setup the cook does keep */
	for (const ALODActor* Actor : Actors) {
		if (Actor == nullptr || !Setup.IsValidIndex(Actor->LODLevel - 1)) continue;

		FHierarchicalSimplification& Level = Setup[Actor->LODLevel - 1];

		Level.bUseOverrideDrawDistance = true;
		Level.OverrideDrawDistance = Actor->GetLODDrawDistance();
	}

	Settings->MarkPackageDirty();

	UE_LOG(LogReflection, Display, TEXT("%d HLOD level(s) said on the world settings"), Levels);
}

void FLevelRead::ReadHLODs(UWorld* World, const TArray<ALODActor*>& Actors, FRLevelReadResult& Result) const {

	/* [4.26] 	Under the 4.26.2 engine, we don't have access to IsEmpty on a TArray.
	 *			Getting Max == 0 *should* return the same functionality. */
	#if !UE4_26
	if (World == nullptr || Actors.IsEmpty()) return;
	#else
	if (World == nullptr || Actors.Max() == 0) return;
	#endif

	/* Which map the proxy belongs to, which the cook keeps nowhere: it is editor-only, so a proxy
	 * that came across names no map and every check the editor makes against it fails */
	TSet<UHLODProxy*> Proxies;

	for (const ALODActor* Actor : Actors) {
		if (Actor != nullptr && Actor->GetProxy() != nullptr) Proxies.Add(Actor->GetProxy());
	}

	for (UHLODProxy* Proxy : Proxies) {
		Proxy->SetMap(World);
		Proxy->MarkPackageDirty();
	}

	/* Kept in the proxy, as this project is set up: the editor remakes each actor from its description, which the cook drops */
	if (!GetDefault<UHierarchicalLODSettings>()->bSaveLODActorsToHLODPackages) {
		return;
	}

	/* Both kept to themselves, so both are reached the way everything else here is reached */
	const FObjectProperty* Held = FindFProperty<FObjectProperty>(ALODActor::StaticClass(), TEXT("ProxyDesc"));
	const FMapProperty* Described = FindFProperty<FMapProperty>(UHLODProxy::StaticClass(), TEXT("HLODActors"));

	if (Held == nullptr || Described == nullptr) return;

	const FObjectProperty* Describes = CastField<FObjectProperty>(Described->KeyProp);
	const FStructProperty* Beside = CastField<FStructProperty>(Described->ValueProp);

	if (Describes == nullptr || Beside == nullptr || Beside->Struct == nullptr) return;

	/* Declared but not exported, so the field is read rather than the accessor called */
	const FNameProperty* Built = FindFProperty<FNameProperty>(Beside->Struct, TEXT("Key"));

	if (Built == nullptr) return;

	/* Every description the proxies hold, against the key the mesh beside it was built under */
	TMap<FName, UObject*> ByKey;

	for (UHLODProxy* Proxy : Proxies) {
		FScriptMapHelper Walk(Described, Described->ContainerPtrToValuePtr<void>(Proxy));

		for (int32 At = 0; At < Walk.GetMaxIndex(); ++At) {
			if (!Walk.IsValidIndex(At)) continue;

			UObject* Desc = Describes->GetObjectPropertyValue(Walk.GetKeyPtr(At));

			if (Desc == nullptr) continue;

			const FName Key = Built->GetPropertyValue_InContainer(Walk.GetValuePtr(At));

			if (!Key.IsNone()) ByKey.Add(Key, Desc);
		}
	}

	/* Which description stands for which actor, before any of it is written down */
	TMap<ALODActor*, UObject*> Tie;

	for (ALODActor* Actor : Actors) {
		if (Actor == nullptr || Actor->GetProxy() == nullptr) continue;

		/* The one thing the cook keeps on both sides, so nothing here has to guess the pairing */
		if (UObject** Standing = ByKey.Find(Actor->GetKey()); Standing != nullptr && *Standing != nullptr) {
			Tie.Add(Actor, *Standing);
		}
	}

	/* And only where everything beneath it can be described too, since the engine stops on a sub actor that has none */
	for (bool bSettled = false; !bSettled; ) {
		bSettled = true;

		for (const TPair<ALODActor*, UObject*>& Pair : Tie) {
			bool bStands = true;

			for (AActor* Under : Pair.Key->SubActors) {
				if (const ALODActor* Below = Cast<ALODActor>(Under); Below != nullptr && !Tie.Contains(Below)) {
					bStands = false;

					break;
				}
			}

			if (!bStands) {
				Tie.Remove(Pair.Key);

				bSettled = false;

				break;
			}
		}
	}

	for (ALODActor* Actor : Actors) {
		if (Actor == nullptr || Actor->GetProxy() == nullptr) continue;

		UObject** Standing = Tie.Find(Actor);

		if (Standing == nullptr) {
			Result.HLODsUntied++;

			continue;
		}

		Held->SetObjectPropertyValue_InContainer(Actor, *Standing);

		Result.HLODsTied++;
	}

	/* Only where every description has an actor: the engine deletes the mesh of one nothing claims */
	TSet<UObject*> Claimed;

	for (const TPair<ALODActor*, UObject*>& Pair : Tie) {
		Claimed.Add(Pair.Value);
	}

	for (UHLODProxy* Proxy : Proxies) {
		bool bWhole = true;

		FScriptMapHelper Walk(Described, Described->ContainerPtrToValuePtr<void>(Proxy));

		for (int32 At = 0; At < Walk.GetMaxIndex() && bWhole; ++At) {
			if (!Walk.IsValidIndex(At)) continue;

			UObject* Desc = Describes->GetObjectPropertyValue(Walk.GetKeyPtr(At));

			bWhole = Desc != nullptr && Claimed.Contains(Desc);
		}

		if (!bWhole) {
			FImportIssues::Report(EImportIssue::Data,
				TEXT("The HLOD proxy was left as it came"),
				FString::Printf(TEXT("\"%s\" describes HLOD actors this level could not account for, and writing it back would take their geometry with it."),
					*Proxy->GetName()));

			continue;
		}

		Proxy->UpdateHLODDescs(World->PersistentLevel);

		if (UPackage* Written = Proxy->GetOutermost(); Written != nullptr && GetSettings()->AssetSettings.SaveAssets) {
			SavePackage(Written);
		}
	}

	UE_LOG(LogReflection, Display, TEXT("%d HLOD actor(s) tied to a description, %d with none, across %d proxy(s)"),
		Result.HLODsTied, Result.HLODsUntied, Proxies.Num());
}

void FLevelRead::ReadSubLevels(const TArray<FString>& Named, FRLevelReadResult& Result) const {
	for (const FString& Package : Named) {
		/* Already here, so it is left alone, which is also what stops a level naming the one that named it */
		if (PackageHoldsAsset(Package)) continue;

		/* Asked for in the game's own spelling, which is the only one Cloud answers to */
		if (TToolImportFromPath::Import(ToCloudPackagePath(Package))) {
			Result.SubLevels++;

			continue;
		}

		Result.MissingSubLevels++;

		FImportIssues::Report(EImportIssue::MissingAsset,
			TEXT("Couldn't read the level ") + FPackageName::GetShortName(Package),
			TEXT("Named at ") + Package);
	}
}

FRLevelReadResult FLevelRead::Read(const TArray<TSharedPtr<FJsonValue>>& Exports, const FString& Path) {
	FRLevelReadResult Result;

	const FRLevelSettings& Options = GetSettings()->Level;

	const FString Name = FPaths::GetBaseFilename(Path);

	/* Everything the read has to say is said about the level, so it reads as one row */
	FImportIssues::Push(Name, Path, TEXT("World"));

	/* The two settings asking for opposite things, which would otherwise read as an empty level */
	if (Options.Actors == ERLevelActors::HLODsOnly && !Options.LODActors) {
		FImportIssues::Report(EImportIssue::Setting,
			TEXT("Nothing will be placed"),
			TEXT("The actors setting asks for HLOD actors only and the LOD Actors switch leaves them out, so the two together allow nothing."));
	}

	/* Made as a level of its own: read into whatever is open, the actors land in somebody else's level */
	FString FailureReason;

	UPackage* Package = FAssetUtilities::CreateAssetPackage(Name, Path, FailureReason);

	if (Package == nullptr) {
		FImportIssues::Report(EImportIssue::Failed, TEXT("Couldn't make a package for the level"), FailureReason);
		FImportIssues::Pop();

		return Result;
	}

	UWorld* World = LevelFor(Package);

	if (World == nullptr) {
		FImportIssues::Report(EImportIssue::Failed, TEXT("Couldn't make a level in that package"));
		FImportIssues::Pop();

		return Result;
	}

	/* Not the one being worked in, which would pull the actors out from under the outliner and the selection */
	if (GEditor != nullptr && World == GEditor->GetEditorWorldContext().World()) {
		FImportIssues::Report(EImportIssue::Failed,
			TEXT("This level is the one open in the editor"),
			TEXT("Open another level first, and read this one again."));

		FImportIssues::Pop();

		return Result;
	}

	SetPackage(Package);

	Result.Level = Package->GetName();

	CreateSerializer();

	FUObjectExportContainer Container(Exports);

	GetObjectSerializer()->Exports = Exports;
	GetObjectSerializer()->GetPropertySerializer()->ExportsContainer = &Container;

	/* A landscape is its heightmap, and none of that is in here: placed, it is empty ground */
	static const TSet<FName> Unreconstructable = { TEXT("Landscape"), TEXT("LandscapeStreamingProxy") };

	/* Every class looked up, with whatever it came back as, so each is asked for once */
	TMap<FString, UClass*> Known;

	/* Placed and filled in two passes, since actors name each other and a level lists them in no order */
	TArray<TPair<FUObjectExport*, AActor*>> Placed;

	/* The merged stand-ins among them, which are tied back to their proxies once all are standing */
	TArray<ALODActor*> HLODs;

	FUObjectExport* WorldExport = nullptr;
	FUObjectExport* LevelExport = nullptr;
	FUObjectExport* WorldSettingsExport = nullptr;

	for (FUObjectExport* Export : Container.Exports) {
		if (!Export->IsJsonValid()) continue;

		if (Export->GetType() == TEXT("World")) WorldExport = Export;
		else if (Export->GetType() == TEXT("Level")) LevelExport = Export;
	}

	/* The two the level keeps for itself, taken from what it names rather than worked out from the
	 * actors: one of them must not be placed and the other must not be fetched to be recognised. */
	FName WorldSettingsNamed;
	FName LevelScriptNamed;

	if (LevelExport != nullptr) {
		const FUObjectJsonValueExport Says = LevelExport->GetPropertiesAsValue();

		WorldSettingsNamed = NamedExport(Says.GetObject(TEXT("WorldSettings")));
		LevelScriptNamed = NamedExport(Says.GetObject(TEXT("LevelScriptActor")));
	}

	for (FUObjectExport* Export : Container.Exports) {
		if (!Export->IsJsonValid()) continue;

		/* An actor sits in the level itself. Everything else in the package sits in one of them, and
		 * the components, graphs and curves below an actor are read as part of reading it. */
		if (Export->GetOuter() != GActorOuter) continue;

		if (Unreconstructable.Contains(Export->GetType())) continue;

		/* What a class keeps as its default, which stands for the class rather than for anything placed */
		if (Export->GetName().ToString().StartsWith(TEXT("Default__"))) continue;

		/* The level's own blueprint, which asking for the class of would import mid-read */
		if (!LevelScriptNamed.IsNone() && Export->GetName() == LevelScriptNamed) continue;

		/* Written onto the settings the level has, at the end: it says nothing about the actors */
		if (!WorldSettingsNamed.IsNone() && Export->GetName() == WorldSettingsNamed) {
			WorldSettingsExport = Export;

			continue;
		}

		bool bMayFetch = false;

		/* Asked before the class is, so nothing is fetched for an actor that is not being placed */
		if (!Allows(Export, bMayFetch)) {
			Result.LeftOut++;

			continue;
		}

		UClass* ActorClass = ClassOf(Export, bMayFetch, Known, Result);

		if (ActorClass == nullptr) {
			if (IsBlueprintMade(Export)) {
				Result.MissingBlueprints++;

				FImportIssues::Report(EImportIssue::MissingClass, TEXT("No class for ") + NamedClass(Export), TEXT("Actors of it were left out of the level"));
			} else {
				Result.UnusableClasses++;

				FImportIssues::Report(EImportIssue::MissingClass, TEXT("This engine has no ") + NamedClass(Export), TEXT("Actors of it were left out of the level"));
			}

			continue;
		}

		/* Only an actor is placed in a level, and a package holds plenty that is not one */
		if (!ActorClass->IsChildOf(AActor::StaticClass())) continue;

		/* There by name and not by anything else, which the spawn would go down over */
		if (!Spawnable(ActorClass)) {
			Result.MissingBlueprints++;

			FImportIssues::Report(EImportIssue::MissingClass,
				TEXT("Nothing was made of ") + NamedClass(Export),
				TEXT("Its class is in memory but was never finished, so actors of it were left out rather than spawned."));

			continue;
		}

		/* The merged stand-ins a level draws at distance, which carry no geometry of their own */
		if (!Options.LODActors && ActorClass->GetName() == TEXT("LODActor")) {
			Result.LeftOut++;

			continue;
		}

		if (Options.Actors == ERLevelActors::StaticMeshesOnly && !ActorClass->IsChildOf(AStaticMeshActor::StaticClass())) {
			Result.LeftOut++;

			continue;
		}

		/* Named as the level names it, or an actor is called after its class and the order it went in */
		FActorSpawnParameters Named;

		Named.Name = Export->GetName();
		Named.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;

		AActor* Actor = World->SpawnActor<AActor>(ActorClass, Named);

		if (Actor == nullptr) {
			Result.Failed++;

			FImportIssues::Report(EImportIssue::Failed, TEXT("The level wouldn't take an actor of ") + ActorClass->GetName());

			continue;
		}

		Actor->SetActorLabel(Export->GetName().ToString());

		Export->Object = Actor;

		Placed.Add(TPair<FUObjectExport*, AActor*>(Export, Actor));
	}

	for (const TPair<FUObjectExport*, AActor*>& Pair : Placed) {
		GetObjectSerializer()->Parent = Pair.Value;

		/* What the level says that the editor works out for itself once the actor is standing */
		GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(Pair.Key->GetProperties(), {
			"MinDrawDistance",
			"Layers",
			"ResourceType"
		}), Pair.Value);

		Pair.Value->Modify();
		Pair.Value->PostEditChange();

		if (Pair.Value->GetRootComponent() != nullptr) {
			Pair.Value->GetRootComponent()->ConditionalUpdateComponentToWorld();
		}

		Pair.Value->PostLoad();

		Result.Placed++;

		if (ALODActor* Stand = Cast<ALODActor>(Pair.Value)) {
			HLODs.Add(Stand);
		}
	}

	/* An attach parent arrives as a pointer, and the engine holds attachment on both sides. Last, since it is read per actor. */
	for (const TPair<FUObjectExport*, AActor*>& Pair : Placed) {
		TInlineComponentArray<USceneComponent*> Components;
		Pair.Value->GetComponents(Components);

		for (USceneComponent* Component : Components) {
			USceneComponent* Above = Component->GetAttachParent();

			if (Above == nullptr || Above == Component || Above->GetAttachChildren().Contains(Component)) continue;

			/* Where it sits was read as an offset from this, so it is the offset that is kept */
			Component->AttachToComponent(Above, FAttachmentTransformRules::KeepRelativeTransform, Component->GetAttachSocketName());
		}
	}

	if (Options.WorldSettings && WorldSettingsExport != nullptr) {
		ReadWorldSettings(World, WorldSettingsExport);
	}

	if (Options.LODActors) {
		ReadHLODSetup(World, HLODs);

		ReadHLODs(World, HLODs, Result);
	}

	/* Collected before the level is written and read after it, since each is a read of its own
	 * and a read in flight would be working in the middle of this one */
	TArray<FString> SubLevels;

	if (Options.SubLevels) {
		SubLevelsNamed(Exports, WorldExport, SubLevels);
	}

	/* The count of what is not standing in it, which a few thousand actors hide by eye */
	if (Result.Missing() > 0 || Result.LeftOut > 0) {
		const int32 Listed = Result.Placed + Result.Missing() + Result.LeftOut;

		FImportIssues::Report(
			Result.Missing() > 0 ? EImportIssue::MissingAsset : EImportIssue::Setting,
			FString::Printf(TEXT("%d of %d actor(s) are not in the level"), Result.Missing() + Result.LeftOut, Listed),
			FString::Printf(TEXT("%d missing a blueprint, %d missing a class, %d the level refused, %d left out by the level settings"),
				Result.MissingBlueprints, Result.UnusableClasses, Result.Failed, Result.LeftOut)
		);
	}

	/* And under what, since a level that came in empty reads like one the settings emptied */
	UE_LOG(LogReflection, Display, TEXT("\"%s\": %d actor(s) placed, %d missing, %d left out (actors=%s, sub levels=%s, world settings=%s, lod actors=%s)"),
		*Name, Result.Placed, Result.Missing(), Result.LeftOut,
		*StaticEnum<ERLevelActors>()->GetNameStringByValue(static_cast<int64>(Options.Actors)),
		Options.SubLevels ? TEXT("on") : TEXT("off"),
		Options.WorldSettings ? TEXT("on") : TEXT("off"),
		Options.LODActors ? TEXT("on") : TEXT("off"));

	/* Handed over the way an asset is, short of the rooting: a level is let go of */
	FAssetRegistryModule::AssetCreated(World);

	World->MarkPackageDirty();
	Package->SetDirtyFlag(true);

	BrowseToAsset(World);

	/* Written out, since a level is opened off disk and an unsaved one left standing is one the open cannot clear */
	if (GetSettings()->AssetSettings.SaveAssets) {
		SavePackage(Package);
	} else {
		FImportIssues::Report(EImportIssue::Setting,
			TEXT("The level was read but not saved"),
			TEXT("Save Assets is off, and a level has to be on disk before it can be opened."));
	}

	FImportIssues::Pop();

	/* And the levels it brings in, once this one is written: each is a read of its own and reports against itself */

	/* [4.26] 	Under the 4.26.2 engine, we don't have access to IsEmpty on a TArray.
	 *			Getting Max == 0 *should* return the same functionality. */
	#if !UE4_26
	if (!SubLevels.IsEmpty()) {
	#else
	if (SubLevels.Max() != 0) {
	#endif
		ReadSubLevels(SubLevels, Result);

		UE_LOG(LogReflection, Display, TEXT("\"%s\" brings in %d level(s): %d read, %d already here, %d could not be"),
			*Name, SubLevels.Num(), Result.SubLevels, SubLevels.Num() - Result.SubLevels - Result.MissingSubLevels,
			Result.MissingSubLevels);
	}

	return Result;
}
