#!/usr/bin/env python3
"""Build the standalone Sequoia release; never install, load, or change EFI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import struct
import subprocess
import sys

from build_common import compiler_flags, sources_from_project

REPO = Path(__file__).resolve().parents[2]
SETTINGS = Path(__file__).resolve().parent
EXCLUDED = {".git", "MacKernelSDK", "build", "DerivedData", "__pycache__",
            "xcuserdata", "project.xcworkspace"}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_manifest(repo=REPO, build_inputs_only=True):
    result = {}
    for directory, children, files in os.walk(repo, followlinks=False):
        children[:] = sorted(n for n in children if n not in EXCLUDED)
        for name in sorted(files):
            path = Path(directory) / name
            if name in {".DS_Store", "FwBinary.cpp"} or name.startswith("._") or name.endswith(".pyc"):
                continue
            relative = path.relative_to(repo)
            if build_inputs_only and (path.suffix == ".md" or relative.parts[0] == ".github"):
                continue
            if path.is_symlink():
                raise ValueError("Source symlink is not supported: " + str(path.relative_to(repo)))
            result[str(relative)] = sha256(path)
    return result


def sdk_manifest():
    sdk = REPO / "MacKernelSDK"
    result = {}
    for name in ("Headers", "Library"):
        for p in sorted((sdk / name).rglob("*")):
            if p.is_symlink():
                raise ValueError("SDK symlink is not supported")
            if p.is_file():
                result[str(p.relative_to(sdk))] = sha256(p)
    return result


def bundle_info(config):
    info = plistlib.loads((REPO / "AirportItlwm/AirportItlwm-Sonoma-Info.plist").read_bytes())
    info.update(CFBundleExecutable=config["executable"], CFBundleIdentifier=config["identifier"],
                CFBundleName=config["executable"], CFBundlePackageType="KEXT",
                CFBundleShortVersionString="0.1.0", CFBundleVersion=config["bundle_version"],
                SequoiaReleaseVersion=config["release_version"],
                SequoiaReleaseConfiguration="Release", SequoiaRequiredBuild=config["required_build"])
    info.pop("OSBundleRequired", None)
    for personality in info["IOKitPersonalities"].values():
        personality["CFBundleIdentifier"] = config["identifier"]
    info["IOKitPersonalities"]["NetworkController"]["IOProbeScore"] = 70000
    info["IOKitPersonalities"]["itlwm"]["IOPCIMatch"] = config["pci_match"]
    # Probe build: the diagnostic personality only attaches when the
    # itlwm-sequoia-firmware boot argument is present, so it stays inert in a
    # normal boot. It publishes the firmware dialogue results as properties
    # instead of creating a network interface.
    info["IOKitPersonalities"]["AirportItlwmSequoiaFirmwareTrial"] = {
        "CFBundleIdentifier": config["identifier"],
        "IOClass": "AirportItlwmSequoiaFirmwareTrial",
        "IOProbeScore": 70000,
        "IOProviderClass": "IOPCIDevice",
        "IOPCIMatch": config["trial_pci_match"],
    }
    return info


def verify_bundle(out):
    config = json.loads((SETTINGS / "release.json").read_text())
    report = json.loads((out / "release-build.json").read_text())
    if report.get("status") != "built-not-boot-validated" or report.get("settings") != config:
        raise ValueError("Release report or settings mismatch")
    bundle = out / (config["executable"] + ".kext")
    binary = bundle / "Contents/MacOS" / config["executable"]
    info = bundle / "Contents/Info.plist"
    paths = [bundle, *bundle.rglob("*")]
    expected = {bundle, bundle / "Contents", bundle / "Contents/MacOS", binary, info}
    if set(paths) != expected or any(p.is_symlink() for p in paths):
        raise ValueError("Unexpected release bundle layout")
    if sha256(binary) != report["executable_sha256"] or sha256(info) != report["info_sha256"]:
        raise ValueError("Release bundle changed")
    if plistlib.loads(info.read_bytes()) != bundle_info(config):
        raise ValueError("Release personality differs")
    magic, cpu, _, kind, count, size, _, _ = struct.unpack("<8I", binary.read_bytes()[:32])
    if (magic, cpu, kind) != (0xFEEDFACF, 0x01000007, 11) or not count or not size:
        raise ValueError("Release is not an x86_64 kext Mach-O")
    return bundle, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=REPO / "build/Sequoia-0.1.0-beta.1-RELEASE")
    args = parser.parse_args()
    out = args.out.resolve()
    if out.is_relative_to(REPO) and not out.is_relative_to(REPO / "build"):
        parser.error("In-repository output must be under build/")
    config = json.loads((SETTINGS / "release.json").read_text())
    lock = json.loads((SETTINGS / "sdk.lock.json").read_text())
    if sdk_manifest() != lock["files"]:
        raise ValueError("SDK differs from sdk.lock.json; follow README-SEQUOIA.md")
    before = source_manifest()
    source_id = hashlib.sha256(json.dumps(before, sort_keys=True).encode()).hexdigest()
    flags = compiler_flags(REPO, "__MAC_15_0", config["release_version"], source_id[:12])
    out.mkdir(parents=True, exist_ok=False)
    objects = out / "objects"
    objects.mkdir()
    report = dict(status="building", settings=config, source_sha256=source_id,
                  sources=before, sdk_lock_sha256=sha256(SETTINGS / "sdk.lock.json"),
                  compiled=0, boot_tested=False, installed=False)

    def run(command, label):
        with (out / (label + ".log")).open("w") as stream:
            subprocess.run(command, cwd=REPO, stdout=stream, stderr=stream, check=True)

    try:
        firmware = objects / "FwBinary.cpp"
        run([sys.executable, str(REPO / "scripts/zlib_compress_fw.py"), str(firmware),
             str(REPO / "itlwm/firmware")], "firmware")
        sources = sources_from_project(REPO)
        sources = [firmware if p.name == "FwBinary.cpp" else p for p in sources]
        # Probe build: the diagnostic personality is not part of the upstream
        # release target, so it is compiled in explicitly here.
        trial_source = REPO / "AirportItlwm/SequoiaFirmwareTrial.cpp"
        if trial_source not in sources:
            sources.append(trial_source)
        compiler = subprocess.check_output(["xcrun", "--find", "clang++"], text=True).strip()
        compiled = []
        for index, source in enumerate(sources):
            obj = objects / (str(index) + ".o")
            run([compiler, *flags, "-c", str(source), "-o", str(obj)], "compile-" + str(index))
            compiled.append(obj)
            report["compiled"] += 1
        module = objects / "SequoiaNativeModule.o"
        run([compiler, *flags, '-DITLWM_NATIVE_MODULE_VERSION="' + config["bundle_version"] + '"',
             "-c", str(REPO / "AirportItlwm/SequoiaNativeModule.cpp"), "-o", str(module)], "module")
        bundle = out / (config["executable"] + ".kext")
        binary = bundle / "Contents/MacOS" / config["executable"]
        binary.parent.mkdir(parents=True)
        info = bundle / "Contents/Info.plist"
        info.write_bytes(plistlib.dumps(bundle_info(config), sort_keys=False))
        run(["xcrun", "ld", "-kext", "-arch", "x86_64", "-macos_version_min", "15.0",
             "-o", str(binary), *map(str, compiled), str(module),
             str(REPO / "MacKernelSDK/Library/x86_64/libkmod.a")], "link")
        run(["xcrun", "strip", "-S", str(binary)], "strip-debug")
        host_build = subprocess.check_output(["sw_vers", "-buildVersion"], text=True).strip()
        report["library_check_exit_code"] = None
        if host_build == config["required_build"]:
            run(["/usr/bin/kmutil", "libraries", "-p", str(bundle), "--undef-symbols"], "libraries")
            report["library_check_exit_code"] = 0
        if before != source_manifest() or sdk_manifest() != lock["files"]:
            raise ValueError("Build inputs changed during compilation")
        report.update(status="built-not-boot-validated", executable_sha256=sha256(binary),
                      info_sha256=sha256(info),
                      uuid=subprocess.check_output(["xcrun", "dwarfdump", "--uuid", str(binary)],
                                                   text=True).split()[1])
    except Exception:
        report["status"] = "failed"
        raise
    finally:
        (out / "release-build.json").write_text(json.dumps(report, indent=2) + "\n")
    verify_bundle(out)
    print(json.dumps({k: report[k] for k in ("status", "compiled", "source_sha256", "uuid",
                     "executable_sha256", "info_sha256", "library_check_exit_code")}, indent=2))


if __name__ == "__main__":
    main()
