/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Types/Texture/TextureTypes.h"

#include "Engine/Compatibility.h"
#include "Dom/JsonObject.h"
#include "CoreMinimal.h"
#include "Serializers/SerializerContainer.h"

/* ReSharper disable once CppUnusedIncludeDirective */
#include "Macros.h"

/* ReSharper disable once CppUnusedIncludeDirective */
#include "TypesHelper.h"

#include "Registry/RegistrationInfo.h"
#include "ImportIssues.h"
#include "Styling/SlateIconFinder.h"
#include "Importers/Constructor/Asset.h"
#include "Engine/Package.h"
#include "Utilities/AssetPaths.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

/* [4.26] 	4.26 doesn't find the UMetaData header on it's own.
 * 			Let's help it out. */
#if UE4_26
#include "UObject/MetaData.h"
#endif

/* The file a package name lands on, if any. */
inline bool PackageFileOf(const FString& LongPackageName, FString& OutFilename) {
#if ENGINE_UE5
	return FPackageName::DoesPackageExist(LongPackageName, &OutFilename);
#else
	return FPackageName::DoesPackageExist(LongPackageName, nullptr, &OutFilename);
#endif
}

/* Whether the project has the asset, rather than merely a file where it would be.
 *
 * An import that stopped partway leaves the package it had already made with nothing in it, and
 * asked only whether the file is there every guard reads that as the project having the asset. */
inline bool PackageHoldsAsset(const FString& LongPackageName) {
	FString Held;

	if (!PackageFileOf(LongPackageName, Held)) return false;

	/* What is in memory is the newest word on it, and an import that just failed is in memory */
	if (const UPackage* Standing = FindPackage(nullptr, *LongPackageName)) {
		TArray<UObject*> Inside;
		GetObjectsWithOuter(Standing, Inside, false);

		for (const UObject* One : Inside) {
			/* Every package carries one of these whether or not anything was built in it */
			if (One != nullptr && !One->IsA<UMetaData>()) return true;
		}

		return false;
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> Found;
	Registry.GetAssetsByPackageName(*LongPackageName, Found, true);

	if (Found.Num() > 0) return true;

	/* Scanned once rather than taken as empty, since a file the registry has not reached answers alike */
	Registry.ScanFilesSynchronous({ Held }, true);
	Registry.GetAssetsByPackageName(*LongPackageName, Found, true);

	return Found.Num() > 0;
}

/* Base handler for converting JSON to assets */
class REFLECTION_API IImporter : public USerializerContainer {
public:
    /* Constructors ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
    IImporter() {}

    virtual ~IImporter() override {}

public:
    /* Overriden in child classes, returns false if failed. */
    virtual bool Import() {
        return false;
    }

    /* Anything worth saying about the asset once it is built, and nothing by default.
     *
     * An asset that imported is not the same as an asset that came across whole: a blackboard can
     * arrive with keys whose type object did not, a font can name typefaces it hasn't got. Those
     * look ordinary in the editor and only misbehave when something reads them, so they are worth
     * a word. Overridden where an asset has a way of being half there.
     *
     * Called as the asset is finished, so nothing has to override Import to say it. */
    virtual void Validate(UObject* Asset) const {}

    virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr);

    template<typename T>
    T* Create() {
        UObject* TargetAsset = CreateAsset(nullptr);

        return Cast<T>(TargetAsset);
    }

public:
    /* Loads a single <T> object ptr */
    template<class T = UObject>
    void LoadExport(const TSharedPtr<FJsonObject>* PackageIndex, TObjectPtr<T>& Object);

    /* Loads an array of <T> object ptrs */
    template<class T = UObject>
    TArray<TObjectPtr<T>> LoadExport(const TArray<TSharedPtr<FJsonValue>>& PackageArray, TArray<TObjectPtr<T>> Array);

public:
    void Save() const;

    /* Handle edit changes, and add it to the content browser */
    bool OnAssetCreation(UObject* Asset) const;
    
    /* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Object Serializer and Property Serializer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
public:
    /* Function to check if an asset needs to be imported. Once imported, the asset will be set and returned. */
    template <class T = UObject>
    FORCEINLINE static TObjectPtr<T> DownloadWrapper(TObjectPtr<T> InObject, FString Type, const FString Name, const FString Path, const FString Lands = FString(), const FString Within = FString());
    /* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Object Serializer and Property Serializer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
};

/* Defined in headers due to symbol errors */
template <class T>
TObjectPtr<T> IImporter::DownloadWrapper(TObjectPtr<T> InObject, FString Type, const FString Name, const FString Path, const FString Lands, const FString Within) {
    const UReflectionSettings* Settings = GetSettings();

    if ((
        InObject == nullptr ||
            (Settings->AssetSettings.Texture.ReflectExistingTextures && Type == "Texture2D")
        )
        && !Path.StartsWith("Engine/") && !Path.StartsWith("/Engine/")
    ) {
        const UObject* DefaultObject = GetClassDefaultObject(T::StaticClass());

        if (DefaultObject != nullptr && !Name.IsEmpty() && !Path.IsEmpty()) {
            bool DownloadStatus = false;

            FString NewPath = Path;
            FRRedirects::Reverse(NewPath);

            /* Asked for a second time by the same import, and already here: a material naming one
             * texture in two slots is the usual way that happens. Only when it is already here,
             * since a reference that resolved to nothing still has to be fetched. */
            const FString Reflected = Type + TEXT("'") + NewPath + TEXT(".") + Name + TEXT("'");

            if (InObject != nullptr && FAssetUtilities::ReflectedThisRun.Contains(Reflected)) {
                return InObject;
            }

            FAssetUtilities::ReflectedThisRun.Add(Reflected);
            
            /* Try importing the asset, saying only the success out loud */
            /* What is asked for and where it ends up are not always the same.
             *
             * Only the game has the thing, and it has it under the name the game gave it, so that
             * is what is asked for. Where it lands is this project's business. */
            const FString Asked = FSoftObjectPath(Type + "'" + NewPath + "." + Name + "'").ToString();
            const FString Where = Lands.IsEmpty() ? Asked : FSoftObjectPath(Type + "'" + Lands + "'").ToString();

            /* And never over one the project already has: a curve inside a blueprint is named through its class, which is not where the loader looks */
            /* Nor one compiled into the build, which is here or nowhere whatever is fetched */
            if (InObject == nullptr && Path.StartsWith(TEXT("/Script/"))) {
                FImportIssues::ReportUnresolvedReference(Type, Name, Path, Within);

                return InObject;
            }

            if (InObject == nullptr && PackageHoldsAsset(FSoftObjectPath(Where).GetLongPackageName())) {
                FImportIssues::ReportUnresolvedReference(Type, Name, Path, Within);

                return InObject;
            }

            if (FAssetUtilities::ConstructAsset(Asked, Where, Type, InObject, DownloadStatus) && DownloadStatus) {
                AppendNotification(
                    FText::FromString(Name),
                    FText::FromString(Type),
                    2.0f,
                    FSlateIconFinder::FindCustomIconBrushForClass(FindObject<UClass>(nullptr, *("/Script/Engine." + Type)), TEXT("ClassThumbnail")),
                    SNotificationItem::CS_Success,
                    false,
                    310.0f
                );
            }

            /* In neither the project nor anywhere Cloud could reach */
            if (InObject == nullptr) {
                FImportIssues::ReportUnresolvedReference(Type, Name, Path, Within);
            }
        }
    }

    return InObject;
}

template <typename T>
void IImporter::LoadExport(const TSharedPtr<FJsonObject>* PackageIndex, TObjectPtr<T>& Object) {
	/* Hefty code */
	FString ObjectType, ObjectName, ObjectPath, Outer;
	PackageIndex->Get()->GetStringField(TEXT("ObjectName")).Split("'", &ObjectType, &ObjectName);

	ObjectPath = PackageIndex->Get()->GetStringField(TEXT("ObjectPath"));
	ObjectPath.Split(".", &ObjectPath, nullptr);

	ObjectName = ObjectName.Replace(TEXT("'"), TEXT(""));

	/* Whether it names something the package holds rather than the package's own asset */
	const bool bWithin = ObjectName.Contains(TEXT(":"));

	/* Kept as the reference spelled it, outer and all, before the name is peeled down to the leaf */
	const FString Spelled = ObjectName;

	/* Inside is inside, however it is spelled.
	 *
	 * A reference names the first thing inside a package after a colon and everything below
	 * that after a dot: Package:Inner.Deeper. Only the dot was peeled, so anything sitting
	 * directly inside a package kept its outer glued to the front of its name and was asked
	 * for as Package.Package:Inner, which is nowhere. */
	ObjectName = ObjectName.Replace(TEXT(":"), TEXT("."));

	/* Subobjects nest arbitrarily deep, so only the last segment names the export and the one
	 * before it is its outer. Peeling a fixed number of segments off the front leaves anything
	 * deeper than two levels unresolvable: a reroute inside a composite's subgraph comes through
	 * as Material:MaterialGraph_1.MaterialGraphNode_Composite_0.<Subgraph>.Reroute_8, and used to
	 * come out of here still carrying "<Subgraph>." in front of its name. */
	if (ObjectName.Contains(".")) {
		FString Chain;
		ObjectName.Split(".", &Chain, &ObjectName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

		/* Leaves Outer alone when there is nothing in front of the leaf but the asset itself */
		Chain.Split(".", nullptr, &Outer, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	}

	ObjectPath = ToEditorPackagePath(ObjectPath);

	/* Something inside a map, which is not an asset and cannot be asked for as one. */
	FString Held;

	if (bWithin && PackageFileOf(ObjectPath, Held) && FPaths::GetExtension(Held, true) == FPackageName::GetMapPackageExtension()) {
		return;
	}

	/* A picture the game kept inside another asset, which has to come out as one of its own.
	 *
	 * A sheet of icons keeps the picture under itself and every sprite cut out of it names it that
	 * way. There is nowhere for it to sit here: what kept it is a shell by the time it is cooked,
	 * with everything about it left behind. So it comes out beside that shell instead, named for
	 * what kept it rather than for the slot it happened to sit in. Only where a package is split
	 * apart: kept together, the picture belongs in it under its own name. */
	FString Lands;

	if (FTextureTypes::IsSupported(ObjectType) && GetSettings()->AssetSettings.PackagedAssets == ERPackagedAssets::Separate) {
		FString Leaf;

		if (ObjectPath.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !Leaf.IsEmpty() && Leaf != ObjectName) {
			Lands = ObjectPath + TEXT("_Texture.") + Leaf + TEXT("_Texture");
		}
	}

	/* Try to load object using the object path and the object name combined */
	TObjectPtr<T> LoadedObject = Lands.IsEmpty() ? nullptr : LoadObjectByPath<T>(Lands);

	/* Asked for under the name it is kept under, which some references spell as a chain and others as the whole path */
	if (!LoadedObject && bWithin) {
		LoadedObject = LoadObjectByPath<T>(Spelled.StartsWith(TEXT("/")) ? Spelled : ObjectPath + TEXT(".") + Spelled);
	}

	/* And where it sat before this, for anything brought in the old way */
	if (!LoadedObject) LoadedObject = LoadObjectByPath<T>(ObjectPath + "." + ObjectName);

	if (!LoadedObject) {
		FString NewObjectPath;
		FString ObjectFileName; {
			ObjectPath.Split("/", &NewObjectPath, &ObjectFileName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		}

		NewObjectPath = NewObjectPath + "/" + ObjectName;

		if (ObjectFileName != ObjectName) {
			LoadedObject = LoadObjectByPath<T>(NewObjectPath + "." + ObjectName);
		}
	}

	if (GetParent() != nullptr) {
		if (!Outer.IsEmpty() && GetParent()->IsA(AActor::StaticClass())) {
			const AActor* NewLoadedObject = Cast<AActor>(GetParent());
			auto Components = NewLoadedObject->GetComponents();
		
			for (UActorComponent* Component : Components) {
				/* TIsDerivedFrom only spelled its result IsDerived before Value was added */
#if UE4_24_BELOW
				if constexpr (TIsDerivedFrom<T, UActorComponent>::IsDerived) {
#else
				if constexpr (TIsDerivedFrom<T, UActorComponent>::Value) {
#endif
					if (ObjectName == Component->GetName()) {
						if (Component->IsA(T::StaticClass())) {
							LoadedObject = Cast<T>(Component);
						}
					}
				}
			}
		}
	}
	
	/* Material Expression case */
	if (!LoadedObject && ObjectName.Contains("MaterialExpression")) {
		FString SplitObjectName;
		ObjectPath.Split("/", nullptr, &SplitObjectName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		LoadedObject = LoadObjectByPath<T>(ObjectPath + "." + SplitObjectName + ":" + ObjectName);
	}

	Object = LoadedObject;

	if (!Object && GetObjectSerializer() != nullptr && GetPropertySerializer() != nullptr && GetPropertySerializer()->ExportsContainer != nullptr) {
		FUObjectExport* Export = GetPropertySerializer()->ExportsContainer->Find(ObjectName);

		/* Named by something read before it was reached, so it is made here rather than fetched from the package already open */
		if (Export != nullptr && Export->IsJsonValid() && Export->Object == nullptr) {
			GetObjectSerializer()->SpawnExport(Export);
		}

		if (Export && Export->IsJsonAndObjectValid() && Export->Object != nullptr && Export->Object->IsA(T::StaticClass())) {
			Object = TObjectPtr<T>(Cast<T>(Export->Object));
		}
	}

	/* If object is still null, send off to Cloud to download */
	if (!Object) {
		if (ObjectType == "WidgetBlueprintGeneratedClass") return;
		
		Object = DownloadWrapper(LoadedObject, ObjectType, ObjectName, ObjectPath, Lands, bWithin ? Spelled : FString());
	}
}

template <typename T>
TArray<TObjectPtr<T>> IImporter::LoadExport(const TArray<TSharedPtr<FJsonValue>>& PackageArray, TArray<TObjectPtr<T>> Array) {
	for (const TSharedPtr<FJsonValue>& ArrayElement : PackageArray) {
		const TSharedPtr<FJsonObject> ObjectPtr = ArrayElement->AsObject();
		TObjectPtr<T> Out;
		
		LoadExport<T>(&ObjectPtr, Out);

		Array.Add(Out);
	}

	return Array;
}