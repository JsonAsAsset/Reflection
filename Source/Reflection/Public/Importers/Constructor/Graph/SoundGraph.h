/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"
#include "Sound/SoundNodeWavePlayer.h"

/* Handles everything needed to create a sound graph from JSON. */
class ISoundGraph : public IImporter {
public:
	/* Graph Functions */
	static void ConnectEdGraphNode(UEdGraphNode* NodeToConnect, UEdGraphNode* NodeToConnectTo, int Pin);
	static void ConnectSoundNode(const USoundNode* NodeToConnect, const USoundNode* NodeToConnectTo, int Pin);

	/* Creates an empty USoundNode, null when this engine has no class for the type */
	static USoundNode* CreateEmptyNode(FName Name, FName Type, USoundCue* SoundCue);

	/* Points out, once a session, that the project never configured its sound groups */
	static void NotifySoundGroupsUnconfigured();

	void ConstructNodes(USoundCue* SoundCue, TMap<FString, USoundNode*>& OutNodes);
	void SetupNodes(const USoundCue* SoundCueAsset, TMap<FString, USoundNode*> SoundCueNodes);
	void OrganizeNodes(const USoundCue* SoundCueAsset);
};
