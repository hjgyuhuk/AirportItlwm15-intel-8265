"""Shared CLT compile settings derived from the verified Sequoia development build."""
import json
from pathlib import Path
import subprocess

def sources_from_project(repo):
    project = json.loads(subprocess.run(
        ["plutil", "-convert", "json", "-o", "-", str(repo / "itlwm.xcodeproj/project.pbxproj")],
        check=True, capture_output=True, text=True).stdout)
    objects = project["objects"]
    paths = {}

    def visit(key, parent):
        item = objects[key]
        tree = item.get("sourceTree", "<group>")
        path = (repo if tree == "SOURCE_ROOT" else parent) / item.get("path", "")
        paths[key] = path
        for child in item.get("children", []):
            visit(child, path)

    visit(objects[project["rootObject"]]["mainGroup"], repo)
    target = next(item for item in objects.values()
                  if item.get("isa") == "PBXNativeTarget" and
                  item.get("name") == "AirportItlwm-Sonoma14.4")
    sources = []
    for phase_key in target["buildPhases"]:
        phase = objects[phase_key]
        if phase["isa"] != "PBXSourcesBuildPhase":
            continue
        for build_key in phase["files"]:
            build = objects[build_key]
            if "fileRef" not in build:
                print(f"Skipping empty upstream PBXBuildFile {build_key}", flush=True)
                continue
            if build.get("settings", {}).get("COMPILER_FLAGS"):
                raise ValueError("Unexpected per-file compiler flags: " + str(build))
            sources.append(paths[build["fileRef"]])
    return sources


def compiler_flags(repo, target, release_version, source_id):
    includes = ["MacKernelSDK/Headers", "itl80211/openbsd", "itl80211/linux",
                "itl80211", "include", "include/Airport", "include/HAL", "AirportItlwm",
                "itlwm/hal_iwm", "itlwm/hal_iwx", "itlwm/hal_iwn"]
    flags = ["-x", "c++", "-std=gnu++14", "-arch", "x86_64", "-O1", "-g0",
             "-mmacosx-version-min=10.15", "-mkernel", "-nostdinc++", "-fapple-kext",
             "-fno-exceptions", "-fno-rtti", "-fno-common", "-fno-builtin",
             "-fno-stack-protector", "-mno-red-zone", "-Wno-deprecated-register",
             "-Werror=return-type", "-DKERNEL", "-DKERNEL_PRIVATE", "-D__KERNEL__",
             "-DAIRPORT", "-D__PRIVATE_SPI__", "-DIO80211FAMILY_V2",
             "-DIEEE80211_STA_ONLY", "-D__IO80211_TARGET=" + target,
             '-DITLWM_VERSION="' + release_version + '"',
             '-DGIT_COMMIT="' + source_id + '"',
             "-ffile-prefix-map=" + str(repo) + "=.",
             "-include", str(repo / "itlwm/PrivateSPI.pch")]
    flags.extend("-I" + str(repo / directory) for directory in includes)
    # Match Xcode's project header map for quoted local headers.
    headers = [str(path.relative_to(repo)) for path in repo.rglob("*")
               if path.is_file() and path.suffix in {".h", ".hpp"}
               and not any(part in {"MacKernelSDK", ".git", "build", "DerivedData"}
                           for part in path.relative_to(repo).parts)]
    for directory in sorted({(repo / header).parent for header in headers}):
        flags.extend(["-iquote", str(directory)])
    return flags

