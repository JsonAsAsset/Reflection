/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "HttpModule.h"
#include "Serialization/JsonSerializer.h"
#include "Settings/ReflectionSettings.h"

class REFLECTION_API Cloud {
public:
	static inline FString URL = TEXT("http://localhost:1500");
	static inline FHttpModule* HttpModule = &FHttpModule::Get();

	/* Status ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
public:
	class REFLECTION_API Status {
	public:
		/* If the Cloud is opened (not if it's ready) */
		static bool IsOpened();

		/* If the Cloud is ready for requests */
		static void IsReady(TFunction<void(bool)> OnResponse);

		/* If the app is not ready or not opened, show the user a notification */
		static void Check(const UReflectionSettings* Settings, TFunction<void(bool)> OnResponse);

		/* Should we wait until the app is initialized? */
		static bool ShouldWaitUntilInitialized(const UReflectionSettings* Settings);
	};

public:
	static void Update(TFunction<void(bool)> OnResponse);
	
	/* Export Endpoints ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
public:
	static inline FString ExportURL = TEXT("/api/export");

	class REFLECTION_API Export {
	public:
		class REFLECTION_API Array {
		public:
			static TArray<TSharedPtr<FJsonValue>> Get(const TMap<FString, FString>& Parameters = {}, const TMap<FString, FString>& Headers = {});
			static TArray<TSharedPtr<FJsonValue>> Get(const FString& Path, const bool Raw, TMap<FString, FString> Parameters = {}, const TMap<FString, FString>& Headers = {});
			static TArray<TSharedPtr<FJsonValue>> GetRaw(const FString& Path, const TMap<FString, FString>& Parameters = {}, const TMap<FString, FString>& Headers = {});
		};
		
		static TSharedPtr<FJsonObject> Get(const TMap<FString, FString>& Parameters = {}, const TMap<FString, FString>& Headers = {});
		static TSharedPtr<FJsonObject> Get(const FString& Path, const bool Raw, TMap<FString, FString> Parameters = {}, const TMap<FString, FString>& Headers = {});
		static TSharedPtr<FJsonObject> GetRaw(const FString& Path, const TMap<FString, FString>& Parameters = {}, const TMap<FString, FString>& Headers = {});

		static void GetRaw(const FString& Path, const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers, TFunction<void(TSharedPtr<FJsonObject>)> OnResponse);

		static void GetAsync(const FString& Path, const bool Raw, TMap<FString, FString> Parameters, const TMap<FString, FString>& Headers, const TFunction<void(TSharedPtr<FJsonObject>)>& OnResponse);
	};

public:
	static auto SendRequest(const FString& RequestURL, const TMap<FString, FString>& Parameters = {}, const TMap<FString, FString>& Headers = {});
	static TSharedPtr<FJsonObject> Get(const FString& RequestURL, const TMap<FString, FString>& Parameters = {}, const TMap<FString, FString>& Headers = {});

	static void Get(
		const FString& RequestURL,
		const TMap<FString, FString>& Parameters,
		const TMap<FString, FString>& Headers,
		TFunction<void(TSharedPtr<FJsonObject>)> OnComplete
	);

	/* Sends a JSON body, and hands back the parsed response along with the HTTP status code.
	 * A code of 0 means the request never made it to the Cloud. */
	static void Post(
		const FString& RequestURL,
		const FString& Body,
		const TMap<FString, FString>& Headers,
		TFunction<void(TSharedPtr<FJsonObject>, int32)> OnComplete
	);

	static TArray<uint8> GetRaw(const FString& RequestURL, const TMap<FString, FString>& Parameters = {}, const TMap<FString, FString>& Headers = {});
	static TArray<TSharedPtr<FJsonValue>> GetExports(const FString& RequestURL, const TMap<FString, FString>& Parameters = {});
};