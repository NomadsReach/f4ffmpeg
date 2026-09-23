# NIF source compilation repair

## Scope

The FFmpeg upgrade branch inherited independent C++ compilation errors in `src/nifHandler.cpp` from upstream `7537e4c12b90b2c268bb3be35a4caf79e6ac7e10`. This repair addresses those source errors without changing engine hooks, CommonLib, FFmpeg configuration, or the current playlist-selection priority.

## Traced failures and repairs

| Location | Failure | Repair |
| --- | --- | --- |
| INI playlist-entry diagnostic | Unescaped quotes terminate the string before `%s`. | Use an escaped, brace-formatted message. |
| Deferred URL, playlist, and render-pass diagnostics | `REX::LOG_DEBUG` and `REX::LOG_ERROR` are not part of the pinned REX logging API. | Use the existing `REX::DEBUG` interface with brace formatting; remove obsolete diagnostics in the replaced resolver block. |
| `resolveLocationPlaybackSettings` / `applyEditorId` | Already-parsed `optional<bool>` and `optional<transitionMethod>` values are passed back through string parsers. The block also references undefined `locId` and `iniPath`. | Apply the typed overrides directly and retain the actual normalized `locationKey`. Keep the pre-resolved transition image path. |
| Standalone INI indexing diagnostic | A duplicated debug message references an out-of-scope `replacement`. | Remove that duplicate; the immediately following INFO message remains. |
| `resolveVideoTarget` | `videoPath` is used without a declaration. An empty initialization alone would lose ordinary direct-video replacements. | Initialize it from `replacement->second.videoPath`, then retain the existing playlist priority. |
| `effectGetRenderPassesHook` diagnostic | `workshopTvEffectKindName` returns `const char*`, so calling `.c_str()` is invalid. | Pass the returned pointer directly to the logger. |

The large existing `nifHandler.cpp` is intentionally not split during this compile repair. The changes remove code and avoid moving unrelated engine-sensitive implementation. The new regression script is under 250 lines.

## Executed proof

Windows run: https://github.com/NomadsReach/f4ffmpeg/actions/runs/35561581100

The run checked the complete repository source, not the abbreviated local reproduction fixture. It verified the original Git blob before applying the candidate patch:

- Original `src/nifHandler.cpp`: `1fd9372d616877e39e7ed6ab1a34fa3655da53fc`
- Repaired source: `dc86fbc385df28ae384c3a9f22c6bb48f22549e1`
- Regression script: `065f31cab42f7f5f8faa6d729fac2806ce00ba38`
- CommonLibF4 revision: `4c6c9fe6bbeae7c753aad8624d91da57f7d2a124`
- CommonLib shared revision: `dfd28a41a832108c4de1367608bd42e1ba477212`

Before the repair, all three extracted production-code groups failed compilation. After the repair, all three compiled with Windows/MSVC and executed successfully, with 24 runtime checks passing. The groups cover the location-override callback, target resolution, and affected log statements.

Coverage includes explicit false overrides, inherited defaults, add/replace/empty playlists, canonical location identity, retained transition paths, unchanged input settings, direct-video fallback, existing global-playlist priority, location-only activation, and invalid texture inputs.

The checked source bytes were uploaded as a Git blob without creating a commit or changing any branch ref. The run verified that the returned blob SHA exactly matched `git hash-object --no-filters` for the tested file. `git diff --check` passed, and submodule status was unchanged before and after the test.

The `nif-source-proof` artifact contains both failing and passing compiler output, generated test translation units, the source diff, and the source-blob record.

## Regression command

From an MSVC developer shell with the repository submodules initialized:

```powershell
python tools/check_nif_source.py --compiler cl --output source-checks
```

The main Windows build runs this check before dependency configuration. Compiler failures therefore surface without waiting for FFmpeg to compile.

## Proof boundaries

The harness compiles production source extracts with the actual `playbackPolicy.h` and pinned `REX/LOG.h`. A temporary standard-library-only `REX/BASE.h` shim isolates the logger header; logging sinks, texture-path canonicalization, and engine-driven location lookup are test substitutes. No dependency files are edited.

This proves the repaired standard C++ paths and logging contract, not the complete plugin translation unit, a full Xmake install, GPU interoperation, or in-game behavior. Full FFmpeg 9.0.2 builds and optional-backend smoke tests remain separate requirements.
