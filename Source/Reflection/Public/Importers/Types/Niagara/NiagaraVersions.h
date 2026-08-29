/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

/* One entry of a Niagara asset's version list, and where it sits */
struct FNiagaraVersion {
	void* Data = nullptr;

	UScriptStruct* Struct = nullptr;

	bool IsValid() const { return Data != nullptr && Struct != nullptr; }
};

/* The version list a Niagara script or emitter keeps itself in.
 *
 * Everything either of them is made of hangs off a version rather than off the asset: the scripts,
 * the graph source, the renderers. The list is not in the export, and the asset keeps it and the
 * entries to itself in C++, so it is reached through the properties instead.
 *
 * What the export does carry is the version it exposes, and the entry made here answers to that
 * name. Left unsaid it is a version nothing asked for, and the asset reports itself corrupt. */
class REFLECTION_API FNiagaraVersions {
public:
	/* The asset's one version, made where the export had none */
	static FNiagaraVersion Only(UObject* Asset);

	/* Every version the asset keeps, in the order it keeps them, and none made.
	 *
	 * An asset holding several is several copies of itself at different points, and what goes
	 * through them goes through all of them rather than only the one it says it is now. */
	static TArray<FNiagaraVersion> All(UObject* Asset);

	/* What a version names there, or nothing */
	static UObject* GetObject(const FNiagaraVersion& Version, const TCHAR* Field);

	static UObject* GetObjectIn(const FNiagaraVersion& Version, const TCHAR* Field, const TCHAR* Inner);

	/* Whether the version already names something there, which it does when its list came across */
	static bool HasObject(const FNiagaraVersion& Version, const TCHAR* Field);

	/* How many things a version lists there, so one that came across whole is left alone */
	static int32 NumObjects(const FNiagaraVersion& Version, const TCHAR* Field);

	static bool SetObject(const FNiagaraVersion& Version, const TCHAR* Field, UObject* Value);

	/* An object sitting inside a struct on the version, the way a script sits in its properties */
	static bool SetObjectIn(const FNiagaraVersion& Version, const TCHAR* Field, const TCHAR* Inner, UObject* Value);

	static int32 SetObjects(const FNiagaraVersion& Version, const TCHAR* Field, const TArray<UObject*>& Values);

	/* Something the version is expected to carry that no export ever wrote.
	 *
	 * Part of what a version holds is the asset's own bookkeeping, made as it makes the version
	 * rather than saved alongside it. Nothing reading a package can find those, and the asset
	 * walks them afterwards without asking whether they are there. One is made of whatever class
	 * the field declares, and only where the field is empty, so anything that did come across is
	 * left as it came. */
	static UObject* MakeObject(const FNiagaraVersion& Version, const TCHAR* Field, UObject* Outer);
};
