/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/Graph/SoundGraph.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/MessageDialog.h"
#include "Modules/Cloud/Cloud.h"
#include "Sound/SoundCue.h"

#include "Engine/Log.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Modules/UI/StyleModule.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the sound wave type used to come in from */
#if UE4_25_BELOW
#include "Sound/SoundWave.h"
#endif
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"

namespace {
	const TCHAR* SoundGroupsSection = TEXT("[/Script/Engine.SoundGroups]");

	/* -1 until the ini files have been read */
	int8 GSoundGroupsDeclared = -1;

	bool GWarnedAboutSoundGroups = false;

	/* BaseEngine.ini ships profiles, so what matters is whether the project declared its own */
	bool ProjectDeclaresSoundGroups() {
		TArray<FString> ConfigFiles;
		IFileManager::Get().FindFilesRecursive(ConfigFiles, *FPaths::ProjectConfigDir(), TEXT("*.ini"), true, false);

		for (const FString& ConfigFile : ConfigFiles) {
			FString Contents;

			if (FFileHelper::LoadFileToString(Contents, *ConfigFile) && Contents.Contains(SoundGroupsSection)) {
				return true;
			}
		}

		return false;
	}

	/* "SoundNodeMixer'/Game/Path/Cue.Cue:SoundNodeMixer_0'" down to the export name the map is keyed by */
	FString ExtractNodeName(const FString& ObjectName) {
		const int32 ColonIndex = ObjectName.Find(TEXT(":"));
		const int32 QuoteIndex = ObjectName.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);

		return ObjectName.Mid(ColonIndex + 1, QuoteIndex - ColonIndex - 1);
	}
}

/* Sound groups are ini-only, with no page in Project Settings to notice them missing on */
void ISoundGraph::NotifySoundGroupsUnconfigured() {
	if (GSoundGroupsDeclared < 0) {
		GSoundGroupsDeclared = ProjectDeclaresSoundGroups() ? 1 : 0;
	}

	if (GSoundGroupsDeclared == 1) {
		return;
	}

	/* Reported every run, since the report is per run */
	FImportIssues::ReportFor(
		TEXT("Project Settings"),
		FString(),
		FString(),
		EImportIssue::Setting,
		TEXT("No [/Script/Engine.SoundGroups] in this project's config"),
		TEXT("Imported audio uses the engine's sound group defaults until the profiles the source game ships with are added to Config/DefaultEngine.ini.")
	);

	if (GWarnedAboutSoundGroups) {
		return;
	}

	GWarnedAboutSoundGroups = true;

	UE_LOG(LogReflection, Warning, TEXT("This project has no %s section in its config, so imported audio falls back on the engine's sound group defaults. Add the profiles the source game uses to Config/DefaultEngine.ini."), SoundGroupsSection);

	/* Not the capped path: said once, and a folder import is when the cap is full */
	AppendNotificationWithHandler(
		FText::FromString("Sound Groups Not Configured"),
		FText::FromString("No [/Script/Engine.SoundGroups] in this project's config, so imported audio uses the engine defaults."),
		10.0f,
		FReflectionStyle::Get().GetBrush("Toolbar.Icon"),
		SNotificationItem::CS_None,
		false,
		420.0f
	);
}

void ISoundGraph::ConstructNodes(USoundCue* SoundCue, TMap<FString, USoundNode*>& OutNodes) {
	for (const FUObjectExport* Export : GetContainer()->Exports) {
		FString Name = Export->GetName().ToString();
		FString Type = Export->GetType().ToString();

		/* Filter only exports with SoundNode at the start */
		if (Type.StartsWith("SoundNode")) {
			USoundNode* SoundCueNode = CreateEmptyNode(FName(*Name), FName(*Type), SoundCue);

			/* Left out rather than stored as a null, which everything downstream would trip on */
			if (SoundCueNode == nullptr) {
				continue;
			}

			OutNodes.Add(Name, SoundCueNode);
		}
	}
}

USoundNode* ISoundGraph::CreateEmptyNode(const FName Name, const FName Type, USoundCue* SoundCue) {
	const UClass* Class = FindClassByType(Type.ToString());

	/* NewObject asserts on a null class, and AllNodes crashes on one that isn't a sound node */
	if (Class == nullptr || !Class->IsChildOf(USoundNode::StaticClass())) {
		UE_LOG(LogReflection, Warning, TEXT("Skipping sound node \"%s\", \"%s\" is not a node type this engine has"), *Name.ToString(), *Type.ToString());

		FImportIssues::Report(
			EImportIssue::MissingClass,
			TEXT("Missing sound node ") + Type.ToString(),
			TEXT("This engine build has no class for it, so the node was left out of the cue.")
		);

		return nullptr;
	}

	/* Set flag to be transactional so it registers with undo system */
	USoundNode* SoundNode = NewObject<USoundNode>(SoundCue, ToNewObjectClass(Class), Name, RF_Transactional);
	SoundCue->AllNodes.Add(SoundNode);
	SoundCue->SetupSoundNode(SoundNode, false);

	return SoundNode;
}

void ISoundGraph::SetupNodes(const USoundCue* SoundCueAsset, TMap<FString, USoundNode*> SoundCueNodes) {
	NotifySoundGroupsUnconfigured();

	/* The graph is built on the cue's first node, so no nodes means nothing to wire together */
	if (SoundCueAsset == nullptr || SoundCueAsset->SoundCueGraph == nullptr) {
		return;
	}

	const TSharedPtr<FJsonObject> AssetProperties = AssetExport->GetProperties();

	/* If Node is connected to Root Node */
	if (AssetProperties.IsValid() && AssetProperties->HasField(TEXT("FirstNode"))) {
		const TSharedPtr<FJsonObject> FirstNodeProp = AssetProperties->TryGetField(TEXT("FirstNode"))->AsObject();

		if (FirstNodeProp.IsValid() && FirstNodeProp->HasTypedField<EJson::String>(TEXT("ObjectName"))) {
			const FString ChildNodeName = ExtractNodeName(FirstNodeProp->GetStringField(TEXT("ObjectName")));

			USoundNode** FirstNode = SoundCueNodes.Find(ChildNodeName);

			/* The root node comes with the graph, so an empty graph has nothing to hang the cue off */
			UEdGraphNode* RootNode =SoundCueAsset->SoundCueGraph->Nodes.IsValidIndex(0) ? SoundCueAsset->SoundCueGraph->Nodes[0] : nullptr;

			/* Connect Node to Root Node */
			if (FirstNode && *FirstNode && RootNode) {
				ConnectEdGraphNode((*FirstNode)->GetGraphNode(), RootNode, 0);
			}
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

		/* Nodes the construction pass skipped never made it into the map */
		if (CurrentNode == nullptr || *CurrentNode == nullptr) {
			continue;
		}

		USoundNode* Node = *CurrentNode;

		/* Filter only node with ChildNodes and handle the pins */
		if (NodeProperties->HasField(TEXT("ChildNodes"))) {
			TArray<TSharedPtr<FJsonValue>> CurrentNodeChildNodes = NodeProperties->TryGetField(TEXT("ChildNodes"))->AsArray();

			/* Save an index of the current connection */
			int32 ConnectionIndex = 0;

			for (const TSharedPtr<FJsonValue>& CurrentNodeValue : CurrentNodeChildNodes) {
				/* An entry that names nothing is an empty input, and still holds its pin */
				const TSharedPtr<FJsonObject> CurrentNodeChildNode =CurrentNodeValue.IsValid() ? CurrentNodeValue->AsObject() : nullptr;

				/* Insert a child node if it doesn't exist */
				if (!Node->ChildNodes.IsValidIndex(ConnectionIndex)) {
					/* InsertChildNode asserts past the end and won't grow a node that is full */
					if (ConnectionIndex > Node->ChildNodes.Num() || Node->ChildNodes.Num() >= Node->GetMaxChildNodes()) {
						UE_LOG(LogReflection, Warning, TEXT("\"%s\" node \"%s\" (%s) lists %d child nodes, the class takes %d - the rest were left unconnected"),
							*GetAssetName(), *Export->GetName().ToString(), *Export->GetType().ToString(), CurrentNodeChildNodes.Num(), Node->GetMaxChildNodes());

						FImportIssues::Report(
							EImportIssue::Data,
							FString::Printf(TEXT("%s lists more child nodes than %s takes"), *Export->GetName().ToString(), *Export->GetType().ToString()),
							FString::Printf(TEXT("%d listed, %d connected - the rest were left off."), CurrentNodeChildNodes.Num(), Node->GetMaxChildNodes())
						);

						break;
					}

					Node->InsertChildNode(ConnectionIndex);
				}

				if (CurrentNodeChildNode.IsValid() && CurrentNodeChildNode->HasTypedField<EJson::String>(TEXT("ObjectName"))) {
					const FString CurrentChildNodeName = ExtractNodeName(CurrentNodeChildNode->GetStringField(TEXT("ObjectName")));

					USoundNode** CurrentChildNode = SoundCueNodes.Find(CurrentChildNodeName);
					const int CurrentPin = ConnectionIndex + 1;

					/* Connect it */
					if (CurrentChildNode && *CurrentChildNode) {
						ConnectSoundNode(*CurrentChildNode, Node, CurrentPin);
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
				FString AssetPtr;

				/* Two shapes for the same thing: the path on its own, or an object carrying it */
				if (NodeProperties->HasTypedField<EJson::String>(TEXT("SoundWaveAssetPtr"))) {
					AssetPtr = NodeProperties->GetStringField(TEXT("SoundWaveAssetPtr"));
				} else {
					const TSharedPtr<FJsonObject> AssetPtrObject = NodeProperties->TryGetField(TEXT("SoundWaveAssetPtr"))->AsObject();

					if (AssetPtrObject.IsValid()) {
						AssetPtrObject->TryGetStringField(TEXT("AssetPathName"), AssetPtr);
					}
				}

				if (AssetPtr.IsEmpty()) {
					continue;
				}

				USoundWave* SoundWave = LoadObjectByPath<USoundWave>(AssetPtr);
				
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

					FString SavePath;

					if (!Response->TryGetStringField(TEXT("file"), SavePath)) continue;

					OnDownloadSoundWave(SavePath, AssetPtr, WavePlayerNode);
				}
			}
		}
	}
}

void ISoundGraph::ConnectEdGraphNode(UEdGraphNode* NodeToConnect, UEdGraphNode* NodeToConnectTo, const int Pin = 1) {
	if (NodeToConnect == nullptr || NodeToConnectTo == nullptr) {
		return;
	}

	/* Pins are built by the audio editor, so a node it never reached has none */
	if (!NodeToConnect->Pins.IsValidIndex(0) || !NodeToConnectTo->Pins.IsValidIndex(Pin)) {
		return;
	}

	NodeToConnect->Pins[0]->MakeLinkTo(NodeToConnectTo->Pins[Pin]);
}

void ISoundGraph::ConnectSoundNode(const USoundNode* NodeToConnect, const USoundNode* NodeToConnectTo, const int Pin = 1) {
	if (NodeToConnect == nullptr || NodeToConnectTo == nullptr) {
		return;
	}

	ConnectEdGraphNode(NodeToConnect->GetGraphNode(), NodeToConnectTo->GetGraphNode(), Pin);
}

void ISoundGraph::OnDownloadSoundWave(const FString& SavePath, FString AssetPtr, USoundNodeWavePlayer* Node) {
	/* Waves reach here on their own as well as through a cue, and they are what the groups act on */
	NotifySoundGroupsUnconfigured();

	if (!FPaths::FileExists(SavePath)) {
		FImportIssues::Report(EImportIssue::MissingAsset, TEXT("Couldn't download a sound wave"), AssetPtr);

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
		USoundWave* SoundWave = LoadObjectByPath<USoundWave>(AssetPtr);
		if (Node) {
			Node->SetSoundWave(SoundWave);
		}
		
		return;
	}
	
	USoundWave* ImportedWave = Cast<USoundWave>(AssetsImported[0]);

	if (!ImportedWave) {
		FImportIssues::Report(EImportIssue::Failed, TEXT("Couldn't reflect a sound wave"), AssetPtr);

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