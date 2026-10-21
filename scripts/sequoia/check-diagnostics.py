#!/usr/bin/env python3
"""Check that the diagnostics are the only thing the release switch turns off.

A function that the switch compiles out must publish properties and log
nothing else. A function that posts to the interface, changes state the
firmware sees, or takes a lock is not a diagnostic, and compiling it out
changes what the driver does. One such function was found the hard way:
the driver-available announcement is a message to the wireless control
layer, and a release build without it can be listed but never joins.

Run before the build. A violation is a build error.
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# What a gated function may not do. Publishing a property and logging are
# what the switch is for; anything here changes the driver's behaviour.
FORBIDDEN = {
    "postMessage": "posts a message to the interface",
    "postMessageSync": "posts a message to the interface",
    "ieee80211_": "changes the network stack's state",
    "iwm_send_cmd": "sends a command to the firmware",
    "iwm_start_device": "starts the radio",
    "iwm_stop_device": "stops the radio",
    "iwm_attach": "attaches the hardware",
    "iwm_detach": "detaches the hardware",
    "iwm_init": "initialises the radio",
    "iwm_resume": "resumes the radio",
    "iwm_stop": "stops the radio",
    "nativeScanStart": "starts a scan",
    "nativeJoinStart": "starts an association",
    "nativeEnable": "enables the radio",
    "nativeDisable": "disables the radio",
}

GATED = re.compile(
    r"#if SEQUOIA_DIAGNOSTICS\n(?P<body>.*?)\n#else\n.*?\n#endif", re.S)
# Every identifier that is called. The name may sit behind an arrow or a
# dot, so it is not anchored to the start of a line.
FUNCTION = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def main():
    violations = []
    for path in sorted(REPO.rglob("*.inc")) + sorted(REPO.rglob("*.cpp")):
        relative = path.relative_to(REPO)
        if relative.parts[0] in {".git", "MacKernelSDK", "build"}:
            continue
        text = path.read_text(errors="replace")
        for block in GATED.finditer(text):
            body = block.group("body")
            names = set(FUNCTION.findall(body))
            for name in sorted(names):
                for forbidden, why in FORBIDDEN.items():
                    if name == forbidden or name.startswith(forbidden):
                        line = text[:block.start()].count("\n") + 1
                        violations.append(
                            f"{relative}:{line}: a gated block calls {name}() ({why})")
    if violations:
        print("A release build would change what the driver does:", file=sys.stderr)
        for violation in violations:
            print("  " + violation, file=sys.stderr)
        return 1
    print("Diagnostics check: every gated block only publishes and logs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
