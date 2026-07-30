/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Tools/ConvexCollision.h"

#include "Engine/StaticMeshSocket.h"
#include "Engine/EngineUtilities.h"

#include "PhysicsEngine/BodySetup.h"
#include "Utilities/JsonUtilities.h"

void TToolConvexCollision::Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) {
	UStaticMesh* StaticMesh = Cast<UStaticMesh>(Object);
	if (StaticMesh == nullptr) return;

	/* Get Body Setup (different in Unreal Engine versions) ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
#if !UE4_27_ONLY_BELOW
	UBodySetup* BodySetup = StaticMesh->GetBodySetup();
#else
	UBodySetup* BodySetup = StaticMesh->BodySetup;
#endif

	for (const TSharedPtr<FJsonValue>& Export : Exports) {
		if (!Export.IsValid() || !Export->AsObject().IsValid()) {
			continue;
		}

		const TSharedPtr<FJsonObject> JsonObject = Export->AsObject();
		if (!IsProperExportData(JsonObject)) continue;

		TSharedPtr<FJsonObject> Properties = JsonObject->GetObjectField(TEXT("Properties"));
		FString Type = JsonObject->GetStringField(TEXT("Type"));

		if (Type == "StaticMesh") {
			StaticMesh->DistanceFieldSelfShadowBias = 0.0;

			/* Create an object serializer */
			StaticMesh->Sockets.Empty();

			GetObjectSerializer()->SetExportForDeserialization(JsonObject, StaticMesh);
			GetObjectSerializer()->Parent = StaticMesh;

			FUObjectExportContainer* Container = new FUObjectExportContainer(Exports);
			GetObjectSerializer()->PropertySerializer->ExportsContainer = Container;
			GetObjectSerializer()->DeserializeExports(Container);

			if (GetObjectSerializer()->GetPropertySerializer()->ExportsContainer) {
				for (const FUObjectExport* UObjectExport : GetObjectSerializer()->GetPropertySerializer()->ExportsContainer->Exports) {
					if (UStaticMeshSocket* Socket = Cast<UStaticMeshSocket>(UObjectExport->Object)) {
						StaticMesh->AddSocket(Socket);
					}
				}
			}

#if ENGINE_UE5
			if (Properties->HasField(TEXT("StaticMaterials"))) {
				const TArray<TSharedPtr<FJsonValue>> StaticMaterials = Properties->GetArrayField(TEXT("StaticMaterials"));

				int MaterialIndex = 0;
				for (FStaticMaterial& StaticMaterial : StaticMesh->GetStaticMaterials()) {
					if (StaticMaterials.IsValidIndex(MaterialIndex))
					{
						const TSharedPtr<FJsonObject> StaticMaterialJsonObject = StaticMaterials[MaterialIndex]->AsObject();

						StaticMaterial.MaterialSlotName = *StaticMaterialJsonObject->GetStringField(TEXT("MaterialSlotName"));
						StaticMaterial.ImportedMaterialSlotName = *StaticMaterialJsonObject->GetStringField(TEXT("ImportedMaterialSlotName"));
					}

					MaterialIndex++;
				}
			}
#endif

			GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(Properties, {
				"StaticMaterials",
				"Sockets"
			}), StaticMesh);
		}

		/* Check if the Class matches BodySetup */
		if (Type != "BodySetup") continue;

		/* Empty any collision data */
		BodySetup->AggGeom.EmptyElements();
		BodySetup->CollisionTraceFlag = CTF_UseDefault;

		GetObjectSerializer()->DeserializeObjectProperties(Properties, BodySetup);

		BodySetup->PostEditChange();

		/* Update physics data */
		BodySetup->InvalidatePhysicsData();
		BodySetup->CreatePhysicsMeshes();

		StaticMesh->MarkPackageDirty();
		StaticMesh->Modify(true);

		/* Notification */
		AppendNotification(
			FText::FromString("Imported SM Data: " + StaticMesh->GetName()),
			FText::FromString(StaticMesh->GetName()),
			3.5f,
			FAppStyle::GetBrush("PhysicsAssetEditor.EnableCollision.Small"),
			SNotificationItem::CS_Success,
			false,
			310.0f
		);
	}
}
