/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Serialization/JsonSerializer.h"
#include "Settings/ReflectionSettings.h"
#include "Modules/Cloud/Remote.h"

/* Talks to the Cloud instance.
 *
 * Everything here is asynchronous unless its name says Blocking. The blocking calls exist for
 * one reason: the serializer discovers asset references while it is deserializing properties and
 * has no continuation to hand a callback to, so it has to wait. They park the game thread, and
 * are only safe inside an FBlockingRequestScope, which keeps the editor drawn while they do.
 *
 * Anything driven by a button, a menu entry or a panel takes a callback instead. */
class REFLECTION_API Cloud {
public:
	static inline FString URL = TEXT("http://localhost:1500");

	/* The profile every request is pinned to, named by its file the way Mirror names one.
	 *
	 * Mirror serves any profile it has mounted, not only the one its window has open, and a
	 * request says which by naming it ahead of the path. Empty, and nothing is named, which is
	 * Mirror answering for whatever it is showing. */
	static inline FString Build;

	/* Status ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
public:
	class REFLECTION_API Status {
	public:
		/* If the Cloud is opened (not if it's ready) */
		static bool IsOpened();

		/* If the Cloud is ready for requests */
		static void IsReady(TFunction<void(bool)> OnResponse);

		/* Says the Cloud isn't running, with the link to the setup */
		static void NotifyNotRunning();

		/* If the app is not ready or not opened, show the user a notification */
		static void Check(TFunction<void(bool)> OnResponse);
	};

public:
	static inline FString MetadataURL = TEXT("/api/metadata");

	static void Update(TFunction<void(bool)> OnResponse);

	/* Fills the runtime in from Cloud's metadata if it isn't there yet, and says whether the
	 * project name ended up set.
	 *
	 * Turning a Cloud path into an editor path needs that name, and the tools reached straight off
	 * a menu never pass through the reflect button that fetches it, so they ask here first. Costs
	 * one request per session. */
	static bool EnsureMetadataBlocking();

	/* Export Endpoints ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
public:
	static inline FString ExportURL = TEXT("/api/export");
	static inline FString SkinWeightsURL = TEXT("/api/export/skinweights");
	static inline FString LodModelURL = TEXT("/api/export/lodmodel");
	static inline FString MorphTargetsURL = TEXT("/api/export/morphtargets");
	static inline FString StaticMeshURL = TEXT("/api/export/staticmesh");
	static inline FString DnaURL = TEXT("/api/export/dna");
	static inline FString ReferenceSkeletonURL = TEXT("/api/export/refskeleton");
	static inline FString ExpressionsURL = TEXT("/api/export/expressions");
	static inline FString FontFaceURL = TEXT("/api/export/fontface");
	static inline FString DnaPosesURL = TEXT("/api/export/dnaposes");
	static inline FString DnaMorphsURL = TEXT("/api/export/dnamorphs");
	static inline FString CurveMappingURL = TEXT("/api/export/curvemapping");

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
		static bool GetBinaryBlocking(const FString& Path, const FString& ContentType, TArray<uint8>& OutData, const FString& Named = FString());

		/* Alternate skin weights, one entry per profile. Not properties, so not in the export:
		 * they live in the LOD's cooked override tables. Empty for anything without them. */
		static TArray<TSharedPtr<FJsonValue>> GetSkinWeightsBlocking(const FString& Path);

		/* The cooked geometry, vertex for vertex. Null when the Cloud has no mesh at Path. */
		static TSharedPtr<FJsonObject> GetLodModelBlocking(const FString& Path);

		/* The cooked morph deltas, keyed to the same vertices the LOD model serves */
		static TSharedPtr<FJsonObject> GetMorphTargetsBlocking(const FString& Path);

		/* The cooked static mesh geometry and its slots. Null when the Cloud has none at Path. */
		/* Named where the package holds more than one mesh, since the Cloud answers with the
		 * first it finds otherwise, and an HLOD proxy keeps a mesh per thing it stands in for */
		static TSharedPtr<FJsonObject> GetStaticMeshBlocking(const FString& Path, const FString& Named = FString());

		/* A MetaHuman head's DNA, as the bytes RigLogic reads. Empty for a mesh without one. */
		static TArray<uint8> GetDnaBlocking(const FString& Path);

		/* The pose the mesh itself is bound at, which is not the one its skeleton carries. Null
		 * when the Cloud has no mesh at Path. */
		static TSharedPtr<FJsonObject> GetReferenceSkeletonBlocking(const FString& Path);

		/* The arithmetic a curve expression asset drives its curves with, which cooking leaves
		 * only as compiled instructions */
		static TSharedPtr<FJsonObject> GetCurveExpressionsBlocking(const FString& Path);

		/* The typeface a font face carries. Everything else about one is a property; this is the
		 * font itself, and nothing else brings it over. */
		static TArray<uint8> GetFontFaceBlocking(const FString& Path);

		/* What each of a head's controls does to its joints, worked out by the Cloud rather than by
		 * RigLogic. The one way to a MetaHuman face on an engine that has no RigLogic at all.
		 *
		 * Asking to backport gets an older head's poses instead, each driving a handful of this
		 * rig's controls at once. Those are evaluated against the mapping where the mapping is, which
		 * is the project the Cloud has open. */
		static TSharedPtr<FJsonObject> GetDnaPosesBlocking(const FString& Path, bool bBackport = false);

		/* What an older head's curves are worth in this rig's controls, as a table.

		 * Which mapping that is, is a fact about the game rather than a choice, so it is not asked
		 * for and cannot be got wrong here: the Cloud has the project open and reads it there. Each
		 * entry is one control and the curves that went into it, weighed by running the expression.

		 * Read forward it says what an older animation becomes and read backwards what a newer one
		 * was made of, which is why it comes over as the table rather than as either answer. */
		static TSharedPtr<FJsonObject> GetCurveMappingBlocking();

		/* The same faces as morph targets: where the vertices end up rather than what the joints did.
		 * A pose asset multiplies rotations together where the rig adds the angles up, which stops
		 * agreeing once a joint turns far, and this face turns lips and eyelids forty degrees. Vertex
		 * offsets just add, so the arithmetic that was wrong stops being in the way. */
		static TSharedPtr<FJsonObject> GetDnaMorphsBlocking(const FString& Path, bool bBackport = false);
	};

	/* Folder Endpoints ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
public:
	static inline FString FolderPathsURL = TEXT("/api/folder/paths");

	class REFLECTION_API Folder {
	public:
		/* Every asset path the Cloud has under Path, subfolders included, in the form the export
		 * endpoint takes back. Empty when the Cloud has nothing there. */
		static TArray<FString> GetPathsBlocking(const FString& Path);
	};

	/* Builds ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
public:
	static inline FString BuildsURL = TEXT("/api/builds");

	/* One of the profiles Mirror has, which is one game's files and the mappings to read them. */
	struct FBuild {
		/* What it is called, which is whatever it was named when it was made */
		FString Name;

		/* The file it is kept in, and the only name two of them cannot share. What every request
		 * about a build names it by. */
		FString Id;

		/* Whether its archives are mounted, which is all it takes for it to answer for an asset */
		bool Loaded = false;

		/* Whether it is the one Mirror has open, which is the one that answers when nothing names
		 * another */
		bool Open = false;

		/* The engine its files were cooked by, as Mirror spells it */
		FString Version;

		int32 Files = 0;
	};

	class REFLECTION_API Builds {
	public:
		/* Every profile Mirror knows of, open or not, mounted or not. Empty when it cannot say,
		 * which is also what a Mirror too old to have been asked this answers. */
		static void ListAsync(TFunction<void(const TArray<FBuild>&)> OnResponse);

		/* Opens one, so Mirror's own window moves to it as well.
		 *
		 * Answers as soon as Mirror has asked its window to move: opening a profile takes however
		 * long its archives take, and that happens after the answer. Whoever asked watches
		 * metadata for the name to come back. */
		static void UseAsync(const FString& Id, TFunction<void(bool)> OnResponse);

		/* Mounts one without opening it, which is all it takes for the editor to read out of it,
		 * and lets a mounted one go again. The open one cannot be let go from here. */
		static void StartAsync(const FString& Id, TFunction<void(bool)> OnResponse);
		static void StopAsync(const FString& Id, TFunction<void(bool)> OnResponse);
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

	/* The same as Post, waited on. OutResponseCode is filled whether or not the body parsed, and
	 * is zero when the request never reached the Cloud. */
	static TSharedPtr<FJsonObject> PostBlocking(const FString& RequestURL, const FString& Body, const TMap<FString, FString>& Headers = {}, int32* OutResponseCode = nullptr);

	/* Drives the requests in flight by hand, and delivers whatever finished.
	 *
	 * A modal window runs a loop that ticks Slate and nothing else, so the engine is not ticking
	 * the HTTP manager and an answer asked for from inside one never arrives. A blocking wait
	 * drives it itself; anything meaning to wait without parking the editor drives it from a
	 * timer, a frame at a time, which is this. */
	static void PumpRequests(float DeltaSeconds);

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
