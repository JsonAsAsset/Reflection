/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Cloud.h"

#include "HttpModule.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Settings/ReflectionSettings.h"
#include "Settings/Runtime.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/RemoteUtilities.h"

TSharedPtr<FJsonObject> Cloud::Export::Get(const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers) {
	return Cloud::Get(ExportURL, Parameters, Headers);
}

void Cloud::Export::GetAsync(const FString& Path, const bool Raw, TMap<FString, FString> Parameters, const TMap<FString, FString>& Headers, const TFunction<void(TSharedPtr<FJsonObject>)>& OnResponse) {
	Parameters.Add(TEXT("path"), Path);
	Parameters.Add(TEXT("raw"), Raw ? TEXT("true") : TEXT("false"));

	Cloud::Get(ExportURL, Parameters, Headers, OnResponse);
}

TSharedPtr<FJsonObject> Cloud::Export::Get(const FString& Path, const bool Raw, TMap<FString, FString> Parameters, const TMap<FString, FString>& Headers) {
	Parameters.Add(TEXT("path"), Path);
	Parameters.Add(TEXT("raw"), Raw ? TEXT("true") : TEXT("false"));
	
	return Get(Parameters, Headers);
}

TSharedPtr<FJsonObject> Cloud::Export::GetRaw(const FString& Path, const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers) {
	return Get(Path, true, Parameters, Headers);
}

void Cloud::Export::GetRaw(const FString& Path, const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers, TFunction<void(TSharedPtr<FJsonObject>)> OnResponse) {
	GetAsync(Path, true, Parameters, Headers,[OnResponse](const TSharedPtr<FJsonObject>& Json) {
		OnResponse(Json);
	});
}

TArray<TSharedPtr<FJsonValue>> Cloud::Export::Array::Get(const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers) {
	const TSharedPtr<FJsonObject> JsonObject = Export::Get(Parameters, Headers);

	if (!JsonObject.IsValid()) return TArray<TSharedPtr<FJsonValue>>();
	
	return JsonObject->GetArrayField(TEXT("exports"));
}

TArray<TSharedPtr<FJsonValue>> Cloud::Export::Array::Get(const FString& Path, const bool Raw, TMap<FString, FString> Parameters, const TMap<FString, FString>& Headers) {
	Parameters.Add(TEXT("path"), Path);
	Parameters.Add(TEXT("raw"), Raw ? TEXT("true") : TEXT("false"));
	
	return Get(Parameters, Headers);
}

TArray<TSharedPtr<FJsonValue>> Cloud::Export::Array::GetRaw(const FString& Path, const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers) {
	return Get(Path, true, Parameters, Headers);
}

auto Cloud::SendRequest(const FString& RequestURL, const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers) {
	const auto NewRequest = HttpModule->CreateRequest();

	FString FullUrl = URL + RequestURL;
	
	if (Parameters.Num() > 0) {
		bool First = true;

		for (const auto& Pair : Parameters) {
			FullUrl += First ? TEXT("?") : TEXT("&");
			First = false;

			FullUrl += FString::Printf(
				TEXT("%s=%s"),
				*FGenericPlatformHttp::UrlEncode(Pair.Key),
				*FGenericPlatformHttp::UrlEncode(Pair.Value)
			);
		}
	}

	for (const auto& Pair : Headers) {
		NewRequest->SetHeader(Pair.Key, Pair.Value);
	}
	
	NewRequest->SetURL(FullUrl);
	
	return NewRequest;
}

TSharedPtr<FJsonObject> Cloud::Get(const FString& RequestURL, const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers) {
	const auto Request = SendRequest(RequestURL, Parameters, Headers);
	Request->SetVerb(TEXT("GET"));

	const auto Response = FRemoteUtilities::ExecuteRequestSync(Request);
	if (!Response.IsValid()) return TSharedPtr<FJsonObject>();

	if (!Response->GetHeader("Content-Type").Contains(TEXT("json"))) {
		return TSharedPtr<FJsonObject>();
	}

	const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
	
	if (TSharedPtr<FJsonObject> JsonObject; FJsonSerializer::Deserialize(JsonReader, JsonObject)) {
		if (Response->GetResponseCode() != 200) {
			return TSharedPtr<FJsonObject>();
		}
		
		return JsonObject;
	}

	return TSharedPtr<FJsonObject>();
}

void Cloud::Get(const FString& RequestURL,
	const TMap<FString, FString>& Parameters,
	const TMap<FString, FString>& Headers,
	TFunction<void(TSharedPtr<FJsonObject>)> OnComplete)
{
	auto Request = SendRequest(RequestURL, Parameters, Headers);
	Request->SetVerb(TEXT("GET"));

	FRemoteUtilities::ExecuteRequestAsync(Request, [OnComplete](auto Response) {
		if (!Response.IsValid()) {
			OnComplete(nullptr);
			return;
		}

		if (!Response->GetHeader("Content-Type").Contains(TEXT("json"))) {
			OnComplete(nullptr);
			return;
		}

		if (Response->GetResponseCode() != 200) {
			OnComplete(nullptr);
			return;
		}

		TSharedPtr<FJsonObject> JsonObject;
		const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(Response->GetContentAsString());

		if (!FJsonSerializer::Deserialize(JsonReader, JsonObject)) {
			OnComplete(nullptr);
			return;
		}

		OnComplete(JsonObject);
	});
}

TArray<uint8> Cloud::GetRaw(const FString& RequestURL, const TMap<FString, FString>& Parameters,
	const TMap<FString, FString>& Headers) {
	const auto Request = SendRequest(RequestURL, Parameters, Headers);
	Request->SetVerb(TEXT("GET"));

	const auto Response = FRemoteUtilities::ExecuteRequestSync(Request);
	if (!Response.IsValid()) return TArray<uint8>();

	return Response->GetContent();
}

TArray<TSharedPtr<FJsonValue>> Cloud::GetExports(const FString& RequestURL, const TMap<FString, FString>& Parameters) {
	const TSharedPtr<FJsonObject> JsonObject = Get(RequestURL, Parameters);
	if (!JsonObject.IsValid()) return {};

	return JsonObject->GetArrayField(TEXT("exports"));
}

void Cloud::Update(TFunction<void(bool)> OnResponse) {
	UReflectionSettings* MutableSettings = GetSettings();
	
	if (!MutableSettings->EnableCloudServer) {
		OnResponse(false);
		
		return;
	}

	Get("/api/metadata", {}, {}, [MutableSettings, OnResponse](const TSharedPtr<FJsonObject>& MetadataResponse) {
		if (!MetadataResponse.IsValid()) {
			OnResponse(false);
			
			return;
		}
		
		if (MetadataResponse->HasField(TEXT("name"))) {
			FString Name = MetadataResponse->GetStringField(TEXT("name"));
			MutableSettings->AssetSettings.ProjectName = Name;
		}

		if (MetadataResponse->HasField(TEXT("major_version"))) {
			const int MajorVersion = MetadataResponse->GetIntegerField(TEXT("major_version"));

			GReflectionRuntime.MajorVersion = MajorVersion;
		}

		if (MetadataResponse->HasField(TEXT("minor_version"))) {
			const int MinorVersion = MetadataResponse->GetIntegerField(TEXT("minor_version"));
				
			GReflectionRuntime.MinorVersion = MinorVersion;
		}

		if (MetadataResponse->HasField(TEXT("profile"))) {
			const auto Profile = MetadataResponse->GetObjectField(TEXT("profile"));

			GReflectionRuntime.Profile.Name = Profile->GetStringField(TEXT("name"));
		}

		SavePluginSettings(MutableSettings);

		OnResponse(true);
	});
}
