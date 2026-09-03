/* Copyright Reflection Contributors 2024-2026 */

#include "Serializers/FoliageActor.h"

#include "Containers/ExportContainer.h"
#include "Engine/Compatibility.h"
#include "Engine/StaticMesh.h"
#include "FoliageType.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Engine/Package.h"
#include "Modules/Toolbar/Tools/ImportFromPath.h"
#include "Serializers/PropertySerializer.h"
#include "Utilities/AssetPaths.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionFoliageActor, All, All);

namespace {
	/* The asset a reference names, asked for where the project has not got it.
	 *
	 * Foliage points at assets of its own, and neither of them is an actor, so nothing else in
	 * reading a level goes near either: the type is a key in the actor's map and the mesh is a
	 * property of a component no actor is made from. Left to the rest of the import they never
	 * arrive, and what grows on the level is dropped for want of somewhere to put it. */
	template <typename T>
	T* LoadReferenced(const FString& CloudPath) {
		if (CloudPath.IsEmpty()) return nullptr;

		const FString Path = ToEditorPackagePath(CloudPath);

		if (T* Found = LoadObjectByPath<T>(Path)) return Found;

		if (!TToolImportFromPath::Import(CloudPath)) return nullptr;

		return LoadObjectByPath<T>(Path);
	}

	/* A reference spelled as a class and a path together, which is how the map keys what grows on
	 * it: the path is what sits between the quotes, and the object at the end of it is dropped
	 * since it names the package again */
	FString PathFromQuoted(const FString& Reference) {
		int32 Open, Close;

		if (!Reference.FindChar(TEXT('\''), Open)) return FString();
		if (!Reference.FindLastChar(TEXT('\''), Close) || Close <= Open) return FString();

		FString Path = Reference.Mid(Open + 1, Close - Open - 1);

		int32 Dot;

		if (Path.FindLastChar(TEXT('.'), Dot)) {
			Path.LeftInline(Dot);
		}

		return Path;
	}

	/* A reference spelled as an object path, which carries the package and then the number of the
	 * export within it */
	FString PathFromObjectPath(const TSharedPtr<FJsonObject>& Reference) {
		FString Path;

		if (!Reference.IsValid() || !Reference->TryGetStringField(TEXT("ObjectPath"), Path)) return FString();

		int32 Dot;

		if (Path.FindLastChar(TEXT('.'), Dot)) {
			Path.LeftInline(Dot);
		}

		return Path;
	}

	/* Every transform a foliage component was carrying.
	 *
	 * They sit beside the component's properties rather than among them, one entry to an instance,
	 * each holding its transform under a name of its own. */
	void ReadInstances(const FUObjectExport* Component, TArray<FTransform>& OutTransforms) {
		if (Component == nullptr || !Component->Has(TEXT("PerInstanceSMData"))) return;

		const TArray<FUObjectJsonValueExport> Instances = Component->GetArray(TEXT("PerInstanceSMData"));

		OutTransforms.Reserve(Instances.Num());

		for (const FUObjectJsonValueExport& Instance : Instances) {
			if (!Instance.JsonObject.IsValid() || !Instance.Has(TEXT("TransformData"))) continue;

			const FUObjectJsonValueExport Data = Instance.GetObject(TEXT("TransformData"));

			if (!Data.Has(TEXT("Rotation")) || !Data.Has(TEXT("Translation")) || !Data.Has(TEXT("Scale3D"))) continue;

			const FUObjectJsonValueExport Rotation = Data.GetObject(TEXT("Rotation"));
			const FUObjectJsonValueExport Translation = Data.GetObject(TEXT("Translation"));
			const FUObjectJsonValueExport Scale = Data.GetObject(TEXT("Scale3D"));

			FQuat Turn(
				Rotation.GetNumber(TEXT("X")),
				Rotation.GetNumber(TEXT("Y")),
				Rotation.GetNumber(TEXT("Z")),
				Rotation.GetNumber(TEXT("W"))
			);

			Turn.Normalize();

			OutTransforms.Add(FTransform(
				Turn,
				FVector(Translation.GetNumber(TEXT("X")), Translation.GetNumber(TEXT("Y")), Translation.GetNumber(TEXT("Z"))),
				FVector(Scale.GetNumber(TEXT("X")), Scale.GetNumber(TEXT("Y")), Scale.GetNumber(TEXT("Z")))
			));
		}
	}
}

void FFoliageActor::Rebuild(AInstancedFoliageActor* Actor, const UPropertySerializer* Serializer) {
	if (Actor == nullptr) return;

	if (Serializer == nullptr || Serializer->ExportsContainer == nullptr) {
		UE_LOG(LogReflectionFoliageActor, Warning, TEXT("\"%s\" was reached without the exports it came from, so nothing is planted"), *Actor->GetName());

		return;
	}

	/* The actor's own export, which is the one that was spawned as it */
	FUObjectExport* Export = nullptr;

	for (FUObjectExport* Candidate : Serializer->ExportsContainer->Exports) {
		if (Candidate != nullptr && Candidate->Object == Actor) {
			Export = Candidate;

			break;
		}
	}

	if (Export == nullptr || !Export->IsJsonValid()) {
		UE_LOG(LogReflectionFoliageActor, Warning, TEXT("\"%s\" is not any of the %d export(s) read, so what it grows cannot be found"), *Actor->GetName(), Serializer->ExportsContainer->Exports.Num());

		return;
	}

	/* Where the actor keeps what grows on it, under the name the engine gave the map before it
	 * stopped writing one and never took the name back */
	if (!Export->Has(TEXT("FoliageMeshes_Deprecated2"))) {
		UE_LOG(LogReflectionFoliageActor, Warning, TEXT("\"%s\" carries no map of what grows on it"), *Actor->GetName());

		return;
	}

	const TSharedPtr<FJsonObject> Meshes = Export->GetObject(TEXT("FoliageMeshes_Deprecated2")).JsonObject;

	if (!Meshes.IsValid()) {
		UE_LOG(LogReflectionFoliageActor, Warning, TEXT("\"%s\" carries a map of what grows on it that could not be read"), *Actor->GetName());

		return;
	}

	UE_LOG(LogReflectionFoliageActor, Display, TEXT("\"%s\" grows %d kind(s)"), *Actor->GetName(), Meshes->Values.Num());

	int32 Kinds = 0;
	int32 Planted = 0;

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Meshes->Values) {
		const TSharedPtr<FJsonObject> Value = Entry.Value.IsValid() ? Entry.Value->AsObject() : nullptr;
		if (!Value.IsValid()) continue;

		const TSharedPtr<FJsonObject>* Reference;
		if (!Value->TryGetObjectField(TEXT("Component"), Reference)) continue;

		FUObjectExport* Component = Serializer->ExportsContainer->GetExportByObjectPath(*Reference);

		if (Component == nullptr || !Component->IsJsonValid()) {
			UE_LOG(LogReflectionFoliageActor, Warning, TEXT("\"%s\" names a component for \"%s\" that is not among the exports"), *Actor->GetName(), *Entry.Key);

			continue;
		}

		TArray<FTransform> Transforms;
		ReadInstances(Component, Transforms);

		if (Transforms.Num() == 0) {
			UE_LOG(LogReflectionFoliageActor, Warning, TEXT("\"%s\" has a component for \"%s\" carrying no instances"), *Actor->GetName(), *Entry.Key);

			continue;
		}

		/* The type the game planted it as, where that asset has been brought in already */
		UFoliageType* Type = LoadReferenced<UFoliageType>(PathFromQuoted(Entry.Key));

		FFoliageInfo* Info = nullptr;

		if (Type != nullptr) {
			Type = Actor->AddFoliageType(Type, &Info);

			/* Registered without handing one back, so it is asked for by name */
			if (Info == nullptr && Type != nullptr) {
				Info = Actor->FindOrAddMesh(Type);
			}
		}

		/* No type to plant it as, so it is planted as its mesh, the way the editor treats anything
		 * dropped into a level without one of its own */
		if (Info == nullptr) {
			const TSharedPtr<FJsonObject> Properties = Component->GetProperties();

			const TSharedPtr<FJsonObject>* MeshReference = nullptr;

			if (Properties.IsValid()) {
				Properties->TryGetObjectField(TEXT("StaticMesh"), MeshReference);
			}

			UStaticMesh* Mesh = MeshReference != nullptr ? LoadReferenced<UStaticMesh>(PathFromObjectPath(*MeshReference)) : nullptr;

			if (Mesh == nullptr) {
				UE_LOG(LogReflectionFoliageActor, Warning, TEXT("\"%s\" grows %d of something neither its type nor its mesh has been brought in for, so none of it is planted"), *Actor->GetName(), Transforms.Num());

				continue;
			}

			/* Makes the type, where GetLocalFoliageTypeForSource only finds one already made */
			Info = Actor->AddMesh(Mesh, &Type);
		}

		if (Type == nullptr || Info == nullptr) {
			UE_LOG(LogReflectionFoliageActor, Warning, TEXT("\"%s\" could not be given anywhere to put its %d of \"%s\""), *Actor->GetName(), Transforms.Num(), *Entry.Key);

			continue;
		}

		for (const FTransform& Transform : Transforms) {
			FFoliageInstance Instance;

			Instance.Location = Transform.GetLocation();
			Instance.Rotation = Transform.GetRotation().Rotator();

#if ENGINE_UE5
			Instance.DrawScale3D = FVector3f(Transform.GetScale3D());
#else
			Instance.DrawScale3D = Transform.GetScale3D();
#endif

#if ENGINE_UE5
			Info->AddInstance(Type, Instance);
#else
			Info->AddInstance(Actor, Type, Instance);
#endif

			Planted++;
		}

		/* Builds the tree the instances are drawn out of, which nothing else does for them */
#if ENGINE_UE5
		Info->Refresh(false, true);
#else
		Info->Refresh(Actor, false, true);
#endif

		Kinds++;
	}

	if (Planted > 0) {
		UE_LOG(LogReflectionFoliageActor, Display, TEXT("\"%s\" was planted with %d instance(s) of %d kind(s)"), *Actor->GetName(), Planted, Kinds);
	}
}
