#!/usr/bin/env python3
"""Validate the exact source and version before publishing a native release."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
SHA = re.compile(r"[0-9a-f]{40}")


def version_tuple(value):
    if not VERSION.fullmatch(value):
        raise ValueError(f"Expected MAJOR.MINOR.PATCH without leading zeros: {value!r}")
    return tuple(map(int, value.split(".")))


def git(*args):
    return subprocess.check_output(["git", *args], text=True).strip()


def prepare(version, expected_sha):
    requested = version_tuple(version)
    if not SHA.fullmatch(expected_sha) or git("rev-parse", "HEAD") != expected_sha:
        raise ValueError("Expected SHA must be the full SHA of the checked-out commit")
    if git("status", "--porcelain", "--untracked-files=no"):
        raise ValueError("Tracked source changes must be committed before release")
    metadata = json.loads(Path("ludeo-release.json").read_text())
    base = metadata["upstream_release"]
    version_tuple(base)
    upstream = metadata["upstream_commit"]
    if not SHA.fullmatch(upstream):
        raise ValueError("Upstream snapshot must be a full commit SHA")
    for ancestor, descendant in [(base + "^{commit}", upstream), (upstream, expected_sha)]:
        subprocess.run(["git", "merge-base", "--is-ancestor", ancestor, descendant], check=True)
    actual_meson = re.search(r"version\s*:\s*'([^']+)'", Path("meson.build").read_text()).group(1)
    if actual_meson != metadata["meson_version"]:
        raise ValueError("Meson version differs from the recorded upstream metadata")
    prefix = base + "-ludeo."
    tag = prefix + version
    for existing in git("tag", "--list", prefix + "*").splitlines():
        if version_tuple(existing.removeprefix(prefix)) > requested:
            raise ValueError(f"Version is older than published tag {existing}")
        if existing == tag and git("rev-parse", f"refs/tags/{tag}^{{commit}}") != expected_sha:
            raise ValueError(f"Tag {tag} already identifies another commit; never move release tags")
    return {
        "schema_version": 1,
        "repository": "https://github.com/EdgeGamingGG/gstreamer",
        **metadata,
        "ludeo_version": version,
        "tag": tag,
        "commit": expected_sha,
        "validation_run": os.environ.get("LUDEO_VALIDATION_RUN", "local validation"),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--expected-sha", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        release = prepare(args.version, args.expected_sha)
    except (ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Release validation failed: {error}\n")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(release, indent=2) + "\n")
    print(release["tag"])
    if "GITHUB_OUTPUT" in os.environ:
        with open(os.environ["GITHUB_OUTPUT"], "a") as output:
            output.write(f"tag={release['tag']}\n")


if __name__ == "__main__":
    main()
