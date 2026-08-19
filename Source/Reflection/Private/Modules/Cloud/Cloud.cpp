/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Cloud.h"

#include "Editor.h"
#include "HttpModule.h"
#include "TimerManager.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Settings/Runtime.h"
#include "Engine/EngineUtilities.h"
#include "Modules/Cloud/Remote.h"

/* Requests ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
FReflectionHttpRequest Cloud::BuildRequest(const FString& RequestURL, const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers) {
	FReflectionHttpRequest NewRequest = FHttpModule::Get().CreateRequest();

	FString FullUrl = URL + RequestURL;

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

	for (const auto& Pair : Headers) {
		NewRequest->SetHeader(Pair.Key, Pair.Value);
	}

	NewRequest->SetURL(FullUrl);

	return NewRequest;
}

TSharedPtr<FJsonObject> Cloud::ParseResponse(const FReflectionHttpResponse& Response) {
	if (!Response.IsValid() || Response->GetResponseCode() != 200) {
		return nullptr;
	}

	if (!Response->GetHeader(TEXT("Content-Type")).Contains(TEXT("json"))) {
		return nullptr;
	}

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(Response->GetContentAsString());

	if (!FJsonSerializer::Deserialize(JsonReader, JsonObject)) {
		return nullptr;
	}

	return JsonObject;
}

void Cloud::Get(const FString& RequestURL,
	const TMap<FString, FString>& Parameters,
	const TMap<FString, FString>& Headers,
	TFunction<void(TSharedPtr<FJsonObject>)> OnComplete)
{
	const FReflectionHttpRequest Request = BuildRequest(RequestURL, Parameters, Headers);
	Request->SetVerb(TEXT("GET"));

	FRemoteUtilities::ExecuteRequestAsync(Request, [OnComplete](const FReflectionHttpResponse& Response) {
		OnComplete(ParseResponse(Response));
	});
}

void Cloud::Post(const FString& RequestURL,
	const FString& Body,
	const TMap<FString, FString>& Headers,
	TFunction<void(TSharedPtr<FJsonObject>, int32)> OnComplete)
{
	const FReflectionHttpRequest Request = BuildRequest(RequestURL, {}, Headers);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(Body);

	FRemoteUtilities::ExecuteRequestAsync(Request, [OnComplete](const FReflectionHttpResponse& Response) {
		/* Zero tells the caller apart from a Cloud that answered but refused */
		const int32 ResponseCode = Response.IsValid() ? Response->GetResponseCode() : 0;

		OnComplete(ParseResponse(Response), ResponseCode);
	});
}

TSharedPtr<FJsonObject> Cloud::GetBlocking(const FString& RequestURL, const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers) {
	const FReflectionHttpRequest Request = BuildRequest(RequestURL, Parameters, Headers);
	Request->SetVerb(TEXT("GET"));

	return ParseResponse(FRemoteUtilities::ExecuteRequestBlocking(Request));
}

void Cloud::RunWhenSafe(TFunction<void()> Work) {
	/* No editor loop to defer onto in the first place */
	if (GEditor == nullptr) {
		Work();

		return;
	}

	/* Timers tick from the editor loop, so next tick is after the HTTP manager tick the response
	 * was delivered from, and after any blocking wait pumping that tick */
	GEditor->GetTimerManager()->SetTimerForNextTick(FTimerDelegate::CreateLambda([Work]() {
		Work();
	}));
}

/* Exports ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
void Cloud::Export::GetAsync(const FString& Path, const bool Raw, TMap<FString, FString> Parameters, const TMap<FString, FString>& Headers, TFunction<void(TSharedPtr<FJsonObject>)> OnResponse) {
	Parameters.Add(TEXT("path"), Path);
	Parameters.Add(TEXT("raw"), Raw ? TEXT("true") : TEXT("false"));

	Cloud::Get(ExportURL, Parameters, Headers, MoveTemp(OnResponse));
}

void Cloud::Export::GetRawAsync(const FString& Path, TFunction<void(TSharedPtr<FJsonObject>)> OnResponse) {
	GetAsync(Path, true, {}, {}, MoveTemp(OnResponse));
}

void Cloud::Export::GetRawExportsAsync(const FString& Path, TFunction<void(const TArray<TSharedPtr<FJsonValue>>&)> OnResponse) {
	GetRawAsync(Path, [OnResponse](const TSharedPtr<FJsonObject>& Response) {
		/* Unreachable Cloud, and a path the Cloud has no export for, are the same to a caller */
		if (!Response.IsValid() || Response->HasField(TEXT("errored"))) {
			OnResponse(TArray<TSharedPtr<FJsonValue>>());

			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Exports;

		if (!Response->TryGetArrayField(TEXT("exports"), Exports)) {
			OnResponse(TArray<TSharedPtr<FJsonValue>>());

			return;
		}

		OnResponse(*Exports);
	});
}

TSharedPtr<FJsonObject> Cloud::Export::GetBlocking(const FString& Path, const bool Raw, TMap<FString, FString> Parameters, const TMap<FString, FString>& Headers) {
	Parameters.Add(TEXT("path"), Path);
	Parameters.Add(TEXT("raw"), Raw ? TEXT("true") : TEXT("false"));

	return Cloud::GetBlocking(ExportURL, Parameters, Headers);
}

TSharedPtr<FJsonObject> Cloud::Export::GetRawBlocking(const FString& Path, const TMap<FString, FString>& Parameters, const TMap<FString, FString>& Headers) {
	return GetBlocking(Path, true, Parameters, Headers);
}

TArray<TSharedPtr<FJsonValue>> Cloud::Export::GetSkinWeightsBlocking(const FString& Path) {
	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(SkinWeightsURL, { { TEXT("path"), Path } }, {});

	/* Unreachable, missing and profile-less all leave the caller with the same nothing */
	if (!Response.IsValid() || !Response->HasField(TEXT("profiles"))) {
		return TArray<TSharedPtr<FJsonValue>>();
	}

	return Response->GetArrayField(TEXT("profiles"));
}

TSharedPtr<FJsonObject> Cloud::Export::GetLodModelBlocking(const FString& Path) {
	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(LodModelURL, { { TEXT("path"), Path } }, {});

	if (!Response.IsValid() || !Response->HasField(TEXT("lods"))) {
		return nullptr;
	}

	return Response;
}

TSharedPtr<FJsonObject> Cloud::Export::GetCurveExpressionsBlocking(const FString& Path) {
	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(ExpressionsURL, { { TEXT("path"), Path } }, {});

	if (!Response.IsValid() || !Response->HasField(TEXT("expressions"))) {
		return nullptr;
	}

	return Response;
}

TSharedPtr<FJsonObject> Cloud::Export::GetReferenceSkeletonBlocking(const FString& Path) {
	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(ReferenceSkeletonURL, { { TEXT("path"), Path } }, {});

	if (!Response.IsValid() || !Response->HasField(TEXT("bones"))) {
		return nullptr;
	}

	return Response;
}

TArray<uint8> Cloud::Export::GetDnaBlocking(const FString& Path) {
	const FReflectionHttpRequest Request = BuildRequest(DnaURL, { { TEXT("path"), Path } }, {});
	Request->SetVerb(TEXT("GET"));

	const FReflectionHttpResponse Response = FRemoteUtilities::ExecuteRequestBlocking(Request);

	if (!Response.IsValid() || Response->GetResponseCode() != 200) {
		return {};
	}

	/* Json means the Cloud answered about the mesh rather than with a DNA */
	if (Response->GetContentType().StartsWith(TEXT("application/json"))) {
		return {};
	}

	return Response->GetContent();
}

TSharedPtr<FJsonObject> Cloud::Export::GetMorphTargetsBlocking(const FString& Path) {
	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(MorphTargetsURL, { { TEXT("path"), Path } }, {});

	if (!Response.IsValid() || !Response->HasField(TEXT("morphs"))) {
		return nullptr;
	}

	return Response;
}

TSharedPtr<FJsonObject> Cloud::Export::GetStaticMeshBlocking(const FString& Path) {
	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(StaticMeshURL, { { TEXT("path"), Path } }, {});

	if (!Response.IsValid() || !Response->HasField(TEXT("lods"))) {
		return nullptr;
	}

	return Response;
}

bool Cloud::Export::GetBinaryBlocking(const FString& Path, const FString& ContentType, TArray<uint8>& OutData) {
	const FReflectionHttpRequest Request = BuildRequest(ExportURL, { { TEXT("path"), Path } }, { { TEXT("content-type"), ContentType } });
	Request->SetVerb(TEXT("GET"));

	const FReflectionHttpResponse Response = FRemoteUtilities::ExecuteRequestBlocking(Request);

	if (!Response.IsValid() || Response->GetResponseCode() != 200) {
		return false;
	}

	/* Json here means the Cloud has the export but couldn't hand back the payload for it */
	if (Response->GetContentType().StartsWith(TEXT("application/json"))) {
		return false;
	}

	OutData = Response->GetContent();

	return OutData.Num() > 0;
}

/* Folders ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
TArray<FString> Cloud::Folder::GetPathsBlocking(const FString& Path) {
	TArray<FString> Paths;

	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(FolderPathsURL, { { TEXT("path"), Path } });
	if (!Response.IsValid() || !Response->HasField(TEXT("paths"))) {
		return Paths;
	}

	for (const TSharedPtr<FJsonValue>& Value : Response->GetArrayField(TEXT("paths"))) {
		FString AssetPath;

		if (Value.IsValid() && Value->TryGetString(AssetPath) && !AssetPath.IsEmpty()) {
			Paths.Add(AssetPath);
		}
	}

	return Paths;
}

/* Metadata ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
static void ApplyMetadata(const TSharedPtr<FJsonObject>& MetadataResponse) {
	/* The project the files came out of, which is what every Cloud path is spelled with */
	if (MetadataResponse->HasField(TEXT("name"))) {
		GReflectionRuntime.Profile.ProjectName = MetadataResponse->GetStringField(TEXT("name"));
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
}

bool Cloud::EnsureMetadataBlocking() {
	if (!GReflectionRuntime.Profile.ProjectName.IsEmpty()) {
		return true;
	}

	const FBlockingRequestScope BlockingScope(NSLOCTEXT("Reflection", "AskingCloudMetadata", "Asking Cloud which project it has loaded"));

	const TSharedPtr<FJsonObject> Response = GetBlocking(MetadataURL);
	if (!Response.IsValid()) {
		return false;
	}

	ApplyMetadata(Response);

	return !GReflectionRuntime.Profile.ProjectName.IsEmpty();
}

void Cloud::Update(TFunction<void(bool)> OnResponse) {
	Get(MetadataURL, {}, {}, [OnResponse](const TSharedPtr<FJsonObject>& MetadataResponse) {
		if (!MetadataResponse.IsValid()) {
			OnResponse(false);

			return;
		}

		ApplyMetadata(MetadataResponse);

		OnResponse(true);
	});
}
