/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Engine/Compatibility.h"
#include "Dom/JsonObject.h"
#include "CoreMinimal.h"
#include "Containers/Serializer.h"

/* ReSharper disable once CppUnusedIncludeDirective */
#include "Macros.h"

/* ReSharper disable once CppUnusedIncludeDirective */
#include "TypesHelper.h"

#include "Registry/RegistrationInfo.h"
#include "Styling/SlateIconFinder.h"
#include "Importers/Constructor/Asset.h"

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

    /*
     * Handle edit changes, and add it to the content browser
     */
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

    if (Settings->EnableCloudServer && (
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
            
            /* Try importing the asset */
            if (FAssetUtilities::ConstructAsset(FSoftObjectPath(Type + "'" + NewPath + "." + Name + "'").ToString(), FSoftObjectPath(Type + "'" + NewPath + "." + Name + "'").ToString(), Type, InObject, DownloadStatus)) {
                const FText AssetNameText = FText::FromString(Name);
                const FSlateBrush* IconBrush = FSlateIconFinder::FindCustomIconBrushForClass(FindObject<UClass>(nullptr, *("/Script/Engine." + Type)), TEXT("ClassThumbnail"));

                if (DownloadStatus) {
                    AppendNotification(
                        AssetNameText,
                        FText::FromString(Type),
                        2.0f,
                        IconBrush,
                        SNotificationItem::CS_Success,
                        false,
                        310.0f
                    );
                } else {
                    AppendNotification(
                        AssetNameText,
                        FText::FromString(Type),
                        5.0f,
                        IconBrush,
                        SNotificationItem::CS_Fail,
                        true,
                        310.0f
                    );
                }
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

	const UReflectionSettings* Settings = GetSettings();

	if (!Settings->AssetSettings.ProjectName.IsEmpty()) {
		ObjectPath = ObjectPath.Replace(*(Settings->AssetSettings.ProjectName + "/Content/"), TEXT("/Game/"));
		ObjectPath = ObjectPath.Replace(*(Settings->AssetSettings.ProjectName + "/Plugins"), TEXT(""));
		ObjectPath = ObjectPath.Replace(TEXT("/Content/"), TEXT("/"));
	}

	ObjectPath = ObjectPath.Replace(TEXT("Engine/Content"), TEXT("/Engine"));
	ObjectName = ObjectName.Replace(TEXT("'"), TEXT(""));

	if (ObjectName.Contains(".")) {
		ObjectName.Split(".", nullptr, &ObjectName);
	}

	if (ObjectName.Contains(".")) {
		ObjectName.Split(".", &Outer, &ObjectName);
	}

	if (!ObjectPath.StartsWith(TEXT("/"))) {
		ObjectPath = "/" + ObjectPath;
	}

	FRRedirects::Redirect(ObjectPath);

	/* Try to load object using the object path and the object name combined */
	TObjectPtr<T> LoadedObject = Cast<T>(StaticLoadObject(T::StaticClass(), nullptr, *(ObjectPath + "." + ObjectName)));

	if (!LoadedObject) {
		FString NewObjectPath;
		FString ObjectFileName; {
			ObjectPath.Split("/", &NewObjectPath, &ObjectFileName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		}

		NewObjectPath = NewObjectPath + "/" + ObjectName;

		if (ObjectFileName != ObjectName) {
			LoadedObject = Cast<T>(StaticLoadObject(T::StaticClass(), nullptr, *(NewObjectPath + "." + ObjectName)));
		}
	}

	if (GetParent() != nullptr) {
		if (!Outer.IsEmpty() && GetParent()->IsA(AActor::StaticClass())) {
			const AActor* NewLoadedObject = Cast<AActor>(GetParent());
			auto Components = NewLoadedObject->GetComponents();
		
			for (UActorComponent* Component : Components) {
				if constexpr (TIsDerivedFrom<T, UActorComponent>::Value) {
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
		LoadedObject = Cast<T>(StaticLoadObject(T::StaticClass(), nullptr, *(ObjectPath + "." + SplitObjectName + ":" + ObjectName)));
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