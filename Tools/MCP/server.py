#!/usr/bin/env python
"""MCP server over Core's Cloud API and the logs either side of it, so a reader can see what
Reflection is actually being served rather than being told about it."""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from mcp.server.mcpserver import MCPServer

CLOUD = os.environ.get("CORE_CLOUD_URL", "http://localhost:1500")
CORE_LOGS = Path(os.environ.get("CORE_LOG_DIR", Path.home() / "AppData/Roaming/Core/.logs"))
EDITOR_LOGS = Path(os.environ.get("EDITOR_LOG_DIR", "")) if os.environ.get("EDITOR_LOG_DIR") else None

server = MCPServer("reflection")


class CloudError(Exception):
    """Something Core said no to, carrying the status so callers can tell 404 from a throw."""

    def __init__(self, message: str, status: int | None = None):
        super().__init__(message)
        self.status = status


def cloud(path: str, build: str | None = None, method: str = "GET", timeout: int = 120):
    """A build named here is put in front of /api, which pins the request to it."""
    root = f"{CLOUD}/{urllib.parse.quote(build)}/api" if build else f"{CLOUD}/api"
    url = f"{root}{path}"

    request = urllib.request.Request(url, method=method)

    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as error:
        body = error.read().decode("utf-8", "replace")

        try:
            detail = json.loads(body).get("errorMessage", "")
        except Exception:
            detail = body[:200]

        raise CloudError(f"{method} {url} answered {error.code}{': ' + detail if detail else ''}", error.code)
    except Exception as error:
        raise CloudError(f"Could not reach Core at {CLOUD} ({error}). Is it running, and is its API enabled?")

    if not body:
        return None

    try:
        return json.loads(body)
    except json.JSONDecodeError:
        return body


def exports_of(path: str, build: str | None):
    """The exports of one asset out of one build, or None where that build has not got it."""
    try:
        body = cloud(f"/export?path={urllib.parse.quote(path)}", build=build)
    except CloudError as error:
        if error.status == 404:
            return None
        raise

    if isinstance(body, dict):
        return body.get("exports") or body.get("jsonOutput") or None

    return body


def trim(value, limit: int) -> str:
    text = value if isinstance(value, str) else json.dumps(value, indent=1)

    if len(text) <= limit:
        return text

    return f"{text[:limit]}\n\n... {len(text) - limit} more characters, narrow the request"


def stamp(entries) -> dict[str, str]:
    """Property names and a short stamp of each value, which is what makes two builds comparable
    without printing two whole exports."""
    out: dict[str, str] = {}

    for entry in entries or []:
        for key, value in (entry.get("Properties") or {}).items():
            text = json.dumps(value)
            out[key] = text[:90] + "…" if len(text) > 90 else text

    return out


def newest_log(directory: Path | None) -> Path | None:
    if not directory or not directory.exists():
        return None

    logs = sorted(directory.glob("*.log"), key=lambda p: p.stat().st_mtime, reverse=True)

    return logs[0] if logs else None


def tail_log(path: Path, lines: int, needle: str) -> str:
    """Logs arrive with null bytes and are far too long to hand over whole."""
    text = path.read_text("latin-1").replace("\x00", "")
    rows = text.splitlines()

    if needle:
        lowered = needle.lower()
        rows = [row for row in rows if lowered in row.lower()]

    return "\n".join(rows[-lines:])


@server.tool()
def builds_list() -> str:
    """Every game build Core knows about, which are loaded, their engine version and file counts.
    Start here when a path or a version is in question."""
    body = cloud("/builds")
    builds = sorted(body["builds"], key=lambda b: (not b["loaded"], b["name"]))

    rows = [
        f"{'LOADED' if b['loaded'] else '      '} {'*' if b['isMain'] else ' '} "
        f"{b['name']:<12} id={b['id']:<8} {b['versionName']}"
        + (f"  files={b['files']}" if b["loaded"] else "")
        for b in builds
    ]

    return (
        f"{len(builds)} build(s), idle unload after {body['idleTimeoutMinutes']} min. "
        f"* is the one Core has open.\n\n" + "\n".join(rows)
    )


@server.tool()
def build_start(build: str) -> str:
    """Load a build so it can be read from. Name it as builds_list shows it, or by its file id
    when two builds share a name."""
    body = cloud(f"/builds/{urllib.parse.quote(build)}/start", method="POST", timeout=900)

    if body.get("alreadyLoaded"):
        return f"{body['name']} was already loaded."

    return f"Loaded {body['name']}: {body.get('files', 0)} files."


@server.tool()
def build_stop(build: str) -> str:
    """Unload a build to release its memory. Refuses the build Core itself has open."""
    body = cloud(f"/builds/{urllib.parse.quote(build)}/stop", method="POST")

    return f"{body['name']} is now {'still loaded' if body['loaded'] else 'unloaded'}."


@server.tool()
def asset_find(query: str, build: str | None = None, under: str | None = None, limit: int = 50) -> str:
    """Find asset paths matching every word of a query. Use this instead of listing a folder,
    which can run to tens of megabytes."""
    params = {"query": query, "limit": str(limit)}

    if under:
        params["under"] = under

    body = cloud(f"/folder/search?{urllib.parse.urlencode(params)}", build=build)

    if not body["total"]:
        return f'Nothing in {body["build"]} matches "{query}".'

    head = f'{body["total"]} match(es) in {body["build"]}'

    if body["truncated"]:
        head += f', first {body["returned"]}'

    return f'{head}:\n\n' + "\n".join(body["paths"])


@server.tool()
def asset_export(
    path: str,
    build: str | None = None,
    fields: list[str] | None = None,
    list_fields: bool = False,
) -> str:
    """What Cloud serves for an asset, which is exactly what Reflection imports from. Pass fields
    to read only the properties you care about rather than the whole export."""
    entries = exports_of(path, build)

    if entries is None:
        return f"{build or 'the open build'} has not got {path}."

    if list_fields:
        blocks = []

        for entry in entries:
            top = [k for k in entry if k not in ("Type", "Name", "Flags", "Class", "Outer")]
            props = list((entry.get("Properties") or {}).keys())

            blocks.append(
                f"{entry.get('Type')} {entry.get('Name')}\n"
                f"  fields: {', '.join(top) or '(none)'}\n"
                f"  properties: {', '.join(props) or '(none)'}"
            )

        return "\n\n".join(blocks)

    if fields:
        kept = []

        for entry in entries:
            picked = {"Type": entry.get("Type"), "Name": entry.get("Name")}

            for field in fields:
                if field in entry:
                    picked[field] = entry[field]

                properties = entry.get("Properties") or {}

                if field in properties:
                    picked.setdefault("Properties", {})[field] = properties[field]

            kept.append(picked)

        entries = kept

    return trim(entries, 12000)


@server.tool()
def cloud_health() -> str:
    """Whether Core can actually serve: which build is open, its engine version, whether mappings
    loaded, unset container keys. Check this first when something fails for no clear reason."""
    meta = cloud("/metadata")
    profile = meta.get("profile") or {}
    notes = []

    if not profile.get("loadedMappingsFile"):
        notes.append("No mappings loaded. Anything unversioned reads as nonsense or not at all.")

    if profile.get("hasErrors"):
        notes.append("The profile reports errors.")

    if profile.get("hasValidationErrors"):
        notes.append("The profile reports validation errors.")

    keys = (profile.get("encryption") or {}).get("keys") or []
    unusable = sum(1 for key in keys if key.get("isValid") is False)

    if unusable:
        notes.append(f"{unusable} of {len(keys)} container key(s) are unset, so those containers cannot be read.")

    try:
        loaded = [b["name"] for b in cloud("/builds")["builds"] if b["loaded"]]
    except CloudError:
        loaded = []

    return "\n".join(
        [
            f"open: {profile.get('name', '(none)')}  engine {meta.get('major_version')}.{meta.get('minor_version')}  version {profile.get('version')}",
            f"project: {meta.get('name', '?')}",
            f"mappings: {profile.get('loadedMappingsFile') or '(none)'}",
            f"archive: {profile.get('archiveDirectory', '?')}",
            f"loaded builds: {', '.join(loaded) or '(none)'}",
            "\nworth knowing:\n- " + "\n- ".join(notes) if notes else "\nnothing obviously wrong.",
        ]
    )


@server.tool()
def asset_diff(path: str, from_build: str, to_build: str) -> str:
    """Compare what two builds serve for the same asset: which properties each has, and where
    values differ. Use when something works in one build and not another."""
    left = exports_of(path, from_build)
    right = exports_of(path, to_build)

    if left is None and right is None:
        return f"Neither {from_build} nor {to_build} has {path}."

    if left is None:
        return f"{from_build} has not got {path}; {to_build} does."

    if right is None:
        return f"{to_build} has not got {path}; {from_build} does."

    a, b = stamp(left), stamp(right)

    only_left = [k for k in a if k not in b]
    only_right = [k for k in b if k not in a]
    differs = [k for k in a if k in b and a[k] != b[k]]

    lines = [
        path,
        f"{from_build}: {len(left)} export(s), {len(a)} propert(ies)",
        f"{to_build}: {len(right)} export(s), {len(b)} propert(ies)",
    ]

    if only_left:
        lines.append(f"\nonly in {from_build}:\n  " + ", ".join(only_left))

    if only_right:
        lines.append(f"\nonly in {to_build}:\n  " + ", ".join(only_right))

    if differs:
        lines.append(f"\ndiffer ({len(differs)}):")

        for key in differs[:25]:
            lines.append(f"  {key}\n    {from_build}: {a[key]}\n    {to_build}: {b[key]}")

        if len(differs) > 25:
            lines.append(f"  ... {len(differs) - 25} more")

    if not (only_left or only_right or differs):
        lines.append("\nidentical.")

    return "\n".join(lines)


@server.tool()
def asset_builds(path: str) -> str:
    """Which of the currently loaded builds actually hold an asset. Answers 'does this version
    have it' without loading anything."""
    loaded = [b for b in cloud("/builds")["builds"] if b["loaded"]]

    if not loaded:
        return "No builds are loaded, so there is nothing to look in."

    rows = []

    for build in loaded:
        found = exports_of(path, build["id"])

        rows.append(f"{'yes' if found else 'no '}  {build['name']:<12} " + (f"{len(found)} export(s)" if found else ""))

    return f"{path}\n\n" + "\n".join(rows) + "\n\n(only loaded builds were checked)"


@server.tool()
def mesh_check(path: str, build: str | None = None) -> str:
    """Whether a mesh's geometry can be read from a build: LOD models, sections, cloth-bound
    sections, skin weights and reference skeleton. This is what Reflection needs to import one."""
    entries = exports_of(path, build)

    if entries is None:
        return f"{build or 'the open build'} has not got {path}."

    mesh = next((e for e in entries if "Mesh" in str(e.get("Type", ""))), entries[0])
    lines = [path, f"type: {mesh.get('Type', 'unknown')}"]
    lods = mesh.get("LODModels")

    if isinstance(lods, list):
        lines.append(f"LODModels: {len(lods)}")

        for index, lod in enumerate(lods):
            sections = (lod or {}).get("Sections") or []
            cloth = sum(1 for s in sections if (s or {}).get("CorrespondClothAssetIndex", -1) >= 0)

            lines.append(f"  LOD{index}: {len(sections)} section(s)" + (f", {cloth} with cloth" if cloth else ""))
    else:
        lines.append("LODModels: absent, the geometry was not read")

    for label, route in (
        ("lodmodel", "/export/lodmodel"),
        ("skinweights", "/export/skinweights"),
        ("refskeleton", "/export/refskeleton"),
    ):
        try:
            cloud(f"{route}?path={urllib.parse.quote(path)}", build=build, timeout=300)
            lines.append(f"{label}: ok")
        except CloudError as error:
            if error.status == 404:
                lines.append(f"{label}: not served")
            elif error.status == 500:
                lines.append(f"{label}: threw, check log_tail")
            else:
                lines.append(f"{label}: {error.status or 'failed'}")

    return "\n".join(lines)


@server.tool()
def log_tail(which: str = "core", lines: int = 80, filter: str = "") -> str:
    """The end of the newest Core or editor log, null bytes stripped. Use filter to pick out
    warnings or a specific asset."""
    directory = EDITOR_LOGS if which == "editor" else CORE_LOGS
    path = newest_log(directory)

    if not path:
        if which == "editor":
            return "No editor log directory is configured. Set EDITOR_LOG_DIR to <project>/Saved/Logs."

        return f"No log found in {directory}."

    return f"{path}\n\n{tail_log(path, lines, filter)}"


if __name__ == "__main__":
    server.run()
