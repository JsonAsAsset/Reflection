/* Copyright Reflection Contributors 2024-2026 */

#pragma once

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
    FORCEINLINE static TObjectPtr<T> DownloadWrapper(TObjectPtr<T> InObject, FString Type, const FString Name, const FString Path);
    /* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Object Serializer and Property Serializer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
};

/* Defined in headers due to symbol errors */
template <class T>
TObjectPtr<T> IImporter::DownloadWrapper(TObjectPtr<T> InObject, FString Type, const FString Name, const FString Path) {
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
            if (FAssetUtilities::ConstructAsset(FSoftObjectPath(Type + "'" + NewPath + "." + Name + "'").ToString(), FSoftObjectPath(Type + "'" + NewPath + "." + Name + "'").ToString(), Type, InObject, DownloadStatus) && DownloadStatus) {
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
                FImportIssues::ReportUnresolvedReference(Type, Name, Path);
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

	/* Try to load object using the object path and the object name combined */
	TObjectPtr<T> LoadedObject = LoadObjectByPath<T>(ObjectPath + "." + ObjectName);

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
		const FUObjectExport* Export = GetPropertySerializer()->ExportsContainer->Find(ObjectName);
		
		if (Export && Export->IsJsonAndObjectValid() && Export->Object != nullptr && Export->Object->IsA(T::StaticClass())) {
			Object = TObjectPtr<T>(Cast<T>(Export->Object));
		}
	}

	/* If object is still null, send off to Cloud to download */
	if (!Object) {
		Object = DownloadWrapper(LoadedObject, ObjectType, ObjectName, ObjectPath);
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