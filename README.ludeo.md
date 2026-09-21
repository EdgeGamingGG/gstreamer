# Ludeo GStreamer fork

`ludeo` is the protected default integration branch for the current GStreamer 1.26-based fork. It starts at `e4cb1d8e339440e73cf37918b8e434e5800a885d`, preserving the earlier Ludeo modifications, 1.26 migration, and NVENC LTR/PTD commits. Keep published history; merge reviewed topic branches without rewriting the integration branch. Preserve upstream release tags. Use `release/1.26` or `release/1.28` for future maintenance lines, since `ludeo` and `ludeo/1.26` cannot coexist as branch names.

## Review and release flow

```text
Topic branch
  -> pull request to ludeo
  -> native build + CPU regression tests + review
  -> merge commit on ludeo
  -> manually dispatch Release Ludeo GStreamer for exact SHA and version
       -> source/version validation
       -> same native build and tests on exact SHA
       -> annotated tag + draft release with provenance and test evidence
       -> publish immutable release
  -> consumer PR records tag and resolved commit SHA
  -> consumer integration build and runtime qualification
```

The release action resolves no moving source after dispatch: its selected `ludeo` commit must equal the full expected SHA. Tests must succeed before any tag is created. Publication is serialized. An existing tag may only be retried at its original commit; an already published release is left unchanged. The action creates and publishes the release itself because pushes made with `GITHUB_TOKEN` do not trigger another push workflow.

Native CI builds RTP, RED/FEC, WebRTC, H.264 parsing, and NVENC on Ubuntu 24.04. It executes the RTX, RED, ULPFEC, RTP bin, RTP funnel, RTP H.264, H.264 parser element, and H.264 parser library suites. NVENC compilation requires no GPU; GPU behavior, full WebRTC sessions, and the exact consumer dependency bundle require application qualification. CI uses system libnice; consumer builds retain their own libnice and Rust plugin pins.

## Version contract

Release tags use `<upstream-release>-ludeo.<major>.<minor>.<patch>`, for example `1.26.11-ludeo.1.0.0`. The Ludeo component describes the downstream public properties, APIs, and behavior: incompatible changes increment major, compatible additions increment minor, and compatible fixes increment patch. The initial baseline is `1.0.0`; optional RTX budget controls introduce `1.1.0`. Versions are compared numerically within the same upstream release base. Upstream upgrades require an explicit metadata review and qualification; do not infer an upstream tag by truncating the runtime version.

These composite tags have SemVer prerelease syntax and sort below the plain upstream release. They are qualified Ludeo releases, but consumers must select exact tags rather than apply generic SemVer ranges across upstream and fork tags. The action explicitly publishes a normal GitHub release without making it an automatic `latest` selector. See <https://semver.org/>.

`ludeo-release.json` records the upstream release ancestor, the exact integrated upstream snapshot, and the native Meson version. The current native version is `1.26.11.1`, including upstream commits after `1.26.11`; the tag's first component identifies the release base, not a claim that all remaining changes are Ludeo changes. Do not replace Meson/runtime versions with composite release tags.

## Creating a release

In GitHub Actions, select **Release Ludeo GStreamer**, choose branch `ludeo`, enter the Ludeo version, and paste the exact full `ludeo` commit SHA. Alternatively:

```bash
gh workflow run ludeo-release.yml --repo EdgeGamingGG/gstreamer --ref ludeo -f ludeo_version=1.0.0 -f expected_sha=<full-reviewed-ludeo-commit>
```

The published release contains `native-release.json`, `native-test-evidence.tar.gz`, and `SHA256SUMS`. Enable repository immutable releases before publication. The release is a source qualification record; binary bundles remain owned by consumer release workflows. Never move a published tag. Fix a bad release with a new version and revert consumers to a previous tag/SHA when necessary.

## Consumer pinning

`ludeocast-streamer` records the human-readable release tag and the resolved native commit, while its Git submodule gitlink remains the checkout authority. CI must verify that the remote tag's peeled commit equals both the recorded commit and the gitlink. `.gitmodules` may name `ludeo` as the development branch, but release builds must not use `git submodule update --remote`. Record the separate `gst-plugins-rs` and `libnice` commit pins in binary provenance.

V1 `LudeoCast-GStreamer` currently uses upstream GStreamer 1.28.6 plus an RTX patch. A 1.26-based native release is not a drop-in replacement. Port and qualify the necessary patches on a separate 1.28 line before changing that build. The cloud image input `ludeocast-gstreamer-ref` identifies the V1 application release, not a tag in this native repository.

## Upstream maintenance

Merge verified upstream stable updates through dedicated PRs. Keep shared branch history intact. For a new upstream series, qualify the required downstream changes on a separate migration branch before changing consumers. Preserve historical release tags and record which downstream patches have become unnecessary.
