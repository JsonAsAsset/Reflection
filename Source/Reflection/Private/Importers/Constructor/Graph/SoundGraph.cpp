/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/SoundGraph.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/MessageDialog.h"
#include "Modules/Cloud/Cloud.h"
#include "Sound/SoundCue.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonUtilities.h"

void ISoundGraph::ConstructNodes(USoundCue* SoundCue, TMap<FString, USoundNode*>& OutNodes) {
	for (const FUObjectExport* Export : GetContainer()->Exports) {
		FString Name = Export->GetName().ToString();
		FString Type = Export->GetType().ToString();

		/* Filter only exports with SoundNode at the start */
		if (Type.StartsWith("SoundNode")) {
			USoundNode* SoundCueNode = CreateEmptyNode(FName(*Name), FName(*Type), SoundCue);

			OutNodes.Add(Name, SoundCueNode);
		}
	}
}

USoundNode* ISoundGraph::CreateEmptyNode(const FName Name, const FName Type, USoundCue* SoundCue) {
	const UClass* Class = FindClassByType(Type.ToString());

	/* Set flag to be transactional so it registers with undo system */
	USoundNode* SoundNode = NewObject<USoundNode>(SoundCue, Class, Name, RF_Transactional);
	SoundCue->AllNodes.Add(SoundNode);
	SoundCue->SetupSoundNode(SoundNode, false);
	
	return SoundNode;
}

void ISoundGraph::SetupNodes(const USoundCue* SoundCueAsset, TMap<FString, USoundNode*> SoundCueNodes) {
	/* If Node is connected to Root Node */
	if (AssetExport->GetProperties()->HasField(TEXT("FirstNode"))) {
		const auto FirstNodeProp = AssetExport->GetProperties()->TryGetField(TEXT("FirstNode"))->AsObject();
		const auto FirstNodeName = FirstNodeProp->TryGetField(TEXT("ObjectName"))->AsString();

		const int32 ColonIndex = FirstNodeName.Find(TEXT(":"));
		const int32 QuoteIndex = FirstNodeName.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		const FString ChildNodeName = FirstNodeName.Mid(ColonIndex + 1, QuoteIndex - ColonIndex - 1);

		USoundNode** FirstNode = SoundCueNodes.Find(ChildNodeName);
		UEdGraphNode* RootNode = SoundCueAsset->SoundCueGraph->Nodes[0];

		/* Connect Node to Root Node */
		if (FirstNode && RootNode) {
			ConnectEdGraphNode((*FirstNode)->GetGraphNode(), RootNode, 0);
		}
	}

	/* Connections done here */
	for (FUObjectExport* Export : GetContainer()->Exports) {
		/* Make sure it has Properties and it's a SoundNode */
		if (!Export->JsonObject->HasField(TEXT("Properties")) || !Export->GetType().ToString().StartsWith("SoundNode")) {
			continue;
		}

		TSharedPtr<FJsonObject> NodeProperties = Export->GetProperties();

		USoundNode** CurrentNode = SoundCueNodes.Find(Export->GetName().ToString());
		USoundNode* Node = *CurrentNode;
		
		/* Filter only node with ChildNodes and handle the pins */
		if (NodeProperties->HasField(TEXT("ChildNodes"))) {
			TArray<TSharedPtr<FJsonValue>> CurrentNodeChildNodes = NodeProperties->TryGetField(TEXT("ChildNodes"))->AsArray();

			/* Save an index of the current connection */
			int32 ConnectionIndex = 0;

			for (TSharedPtr<FJsonValue> CurrentNodeValue : CurrentNodeChildNodes) {
				const auto CurrentNodeChildNode = CurrentNodeValue->AsObject();

				/* Insert a child node if it doesn't exist */
				if (!Node->ChildNodes.IsValidIndex(ConnectionIndex)) {
					Node->InsertChildNode(ConnectionIndex);
				}

				if (CurrentNodeChildNode->HasField(TEXT("ObjectName"))) {
					auto CurrentChildNodeObjectName = CurrentNodeChildNode->TryGetField(TEXT("ObjectName"))->AsString();

					const int32 ColonIndex = CurrentChildNodeObjectName.Find(TEXT(":"));
					const int32 QuoteIndex = CurrentChildNodeObjectName.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
					FString CurrentChildNodeName = CurrentChildNodeObjectName.Mid(ColonIndex + 1, QuoteIndex - ColonIndex - 1);

					USoundNode** CurrentChildNode = SoundCueNodes.Find(CurrentChildNodeName);
					const int CurrentPin = ConnectionIndex + 1;

					/* Connect it */
					if (CurrentNode && CurrentChildNode) {
						ConnectSoundNode(*CurrentChildNode, *CurrentNode, CurrentPin);
					}
				}
				
				ConnectionIndex++;
			}
		}

		/* Deserialize Node Properties */
		GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(NodeProperties, TArray<FString> {
			"ChildNodes"
		}), *CurrentNode);

		/* Import Sound Wave */
		if (Cast<USoundNodeWavePlayer>(Node) != nullptr) {
			const auto WavePlayerNode = Cast<USoundNodeWavePlayer>(Node);

			if (NodeProperties->HasField(TEXT("SoundWaveAssetPtr"))) {
				FString AssetPtr = NodeProperties->TryGetField(TEXT("SoundWaveAssetPtr"))->AsObject()->GetStringField(TEXT("AssetPathName"));

				if (NodeProperties->HasTypedField<EJson::String>(TEXT("SoundWaveAssetPtr"))) {
					AssetPtr = NodeProperties->GetStringField(TEXT("SoundWaveAssetPtr"));
				}

				USoundWave* SoundWave = Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *AssetPtr));
				
				/* Already exists */
				if (SoundWave != nullptr) {
					WavePlayerNode->SetSoundWave(SoundWave);
				} else {
					const TSharedPtr<FJsonObject> Response = Cloud::Export::GetRawBlocking(AssetPtr, {
						{
							"save",
							"true"
						}
					});
					
					if (Response == nullptr) continue;
					
					OnDownloadSoundWave(Response->GetStringField(TEXT("file")), AssetPtr, WavePlayerNode);
				}
			}
		}
	}
}

void ISoundGraph::ConnectEdGraphNode(UEdGraphNode* NodeToConnect, UEdGraphNode* NodeToConnectTo, const int Pin = 1) {
	NodeToConnect->Pins[0]->MakeLinkTo(NodeToConnectTo->Pins[Pin]);
}

void ISoundGraph::ConnectSoundNode(const USoundNode* NodeToConnect, const USoundNode* NodeToConnectTo, const int Pin = 1) {
	if (NodeToConnectTo->GetGraphNode()->Pins.IsValidIndex(Pin)) {
		NodeToConnect->GetGraphNode()->Pins[0]->MakeLinkTo(NodeToConnectTo->GetGraphNode()->Pins[Pin]);
	}
}

void ISoundGraph::OnDownloadSoundWave(const FString& SavePath, FString AssetPtr, USoundNodeWavePlayer* Node) {
	if (!FPaths::FileExists(SavePath)) {
		AppendNotification(
			FText::FromString("Failed: " + AssetPtr),
			FText::FromString("Failed to download sound wave"),
			8.0f,
			SNotificationItem::ECompletionState::CS_Fail,
			true,
			456.0
		);
		return;
	}

	IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	ImportData->Filenames.Add(SavePath);
	
	FRRedirects::Redirect(AssetPtr);
	
	ImportData->DestinationPath = FPaths::GetPath(AssetPtr);
	ImportData->bReplaceExisting = true;
	
	auto AssetsImported = AssetTools.ImportAssetsAutomated(ImportData);
	if (!AssetsImported.IsValidIndex(0)) {
		USoundWave* SoundWave = Cast<USoundWave>(StaticLoadObject(USoundWave::StaticClass(), nullptr, *AssetPtr));
		if (Node) {
			Node->SetSoundWave(SoundWave);
		}
		
		return;
	}
	
	USoundWave* ImportedWave = Cast<USoundWave>(AssetsImported[0]);

	if (!ImportedWave) {
		AppendNotification(
			FText::FromString("Failed: " + AssetPtr),
			FText::FromString("Failed to reflect sound wave"),
			8.0f,
			SNotificationItem::ECompletionState::CS_Fail,
			true,
			456.0
		);
		return;
	}

	ImportedWave->AssetImportData = nullptr;

	if (Node) {
		Node->SetSoundWave(ImportedWave);
	}

	const FString Type = "SoundWave";
	const FSlateBrush* IconBrush = FSlateIconFinder::FindCustomIconBrushForClass(FindObject<UClass>(nullptr, *("/Script/Engine." + Type)), TEXT("ClassThumbnail"));

	AppendNotification(
		FText::FromString("Sound Downloaded: " + ImportedWave->GetName()),
		FText::FromString(""),
		2.0f,
		IconBrush,
		SNotificationItem::CS_Success,
		false,
		310.0f
	);
}