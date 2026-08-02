/* Copyright Reflection Contributors 2024-2026 */

#pragma once

struct ImportTypes {
	/* AssetType/Category ~ Defined in CPP */
	static TMap<FString, TArray<FString>> Templated;
	
	struct Cloud {
		static inline TArray<FString> Blacklisted = {
			"AnimSequence",
			"AnimMontage",
			"AnimBlueprintGeneratedClass",
			"BlueprintGeneratedClass",
			"WidgetBlueprintGeneratedClass",
		};

		static bool Allowed(const FString& Type);
	};

	static inline TArray<FString> Experimental = {
		"AnimBlueprintGeneratedClass",
		"BlueprintGeneratedClass",
		"WidgetBlueprintGeneratedClass"
	};

	static bool Allowed(const FString& ImporterType);
};