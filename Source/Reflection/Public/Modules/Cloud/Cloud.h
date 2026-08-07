/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Serialization/JsonSerializer.h"
#include "Settings/ReflectionSettings.h"
#include "Modules/Cloud/Remote.h"

/*
 * Talks to the local Cloud instance.
 *
 * Everything here is asynchronous unless its name says Blocking. The blocking calls exist for
 * one reason: the serializer discovers asset references while it is deserializing properties and
 * has no continuation to hand a callback to, so it has to wait. They park the game thread, and
 * are only safe inside an FBlockingRequestScope, which keeps the editor drawn while they do.
 *
 * Anything driven by a button, a menu entry or a panel takes a callback instead.
 */
class REFLECTION_API Cloud {
public:
	static inline FString URL = TEXT("http://localhost:1500");

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
		static void GetAsync(const FString& Path, bool Raw, TMap<FString, FString> Parameters, const TMap<FString, FString>& Headers, TFunction<void(TSharedPtr<FJsonObject>)> OnResponse);
		static void GetRawAsync(const FString& Path, TFunction<void(TSharedPtr<FJsonObject>)> OnResponse);

		/* Hands back the export list for Path, empty if the Cloud has nothing for it. What
		 * every Cloud tool actually wants out of a raw export request. */
		static void GetRawExportsAsync(const FString& Path, TFunction<void(const TArray<TSharedPtr<FJsonValue>>&)> OnResponse);

		static TSharedPtr<FJsonObject> GetBlocking(const FString& Path, bool Raw, TMap<FString, FString> Parameters = {}, const TMap<FString, FString>& Headers = {});
		static TSharedPtr<FJsonObject> GetRawBlocking(const FString& Path, const TMap<FString, FString>& Parameters = {}, const TMap<FString, FString>& Headers = {});

		/* Asks the export endpoint for bytes rather than json, with ContentType picking the
		 * encoding. False when the Cloud couldn't produce the payload, which it says by answering
		 * with json instead. */
		static bool GetBinaryBlocking(const FString& Path, const FString& ContentType, TArray<uint8>& OutData);
	};

	/* Requests ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
public:
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

	static TSharedPtr<FJsonObject> GetBlocking(const FString& RequestURL, const TMap<FString, FString>& Parameters = {}, const TMap<FString, FString>& Headers = {});

	/* Runs Work on the next editor tick, off whatever call stack the response arrived on.
	 *
	 * Responses are delivered from the HTTP manager's tick, so a callback runs inside HTTP
	 * request finalization, and a blocking wait drives that tick itself, so it can also land in
	 * the middle of an unrelated import. Anything that creates or edits assets, or drives the
	 * Content Browser, goes through here to get a call stack of its own. */
	static void RunWhenSafe(TFunction<void()> Work);

private:
	static FReflectionHttpRequest BuildRequest(const FString& RequestURL, const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers);

	/* Shared response handling for both Get flavours: a response is only usable if it arrived,
	 * came back 200, says it is JSON, and parses. */
	static TSharedPtr<FJsonObject> ParseResponse(const FReflectionHttpResponse& Response);
};
