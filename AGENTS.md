# Agent guidelines

## Product and layout
- ApexReplay is a Windows-only Apex replay recorder; the UI is WPF/.NET in `src/ApexPerif.App`.
- The C++20 worker in `src/native` captures video/audio, detects HUD events and exports clips.
- UI and worker communicate through a named pipe; check existing message shapes and APIs before editing.
- Portable headers: `image.hpp`, `names.hpp`, `digits.hpp`, `hud_tracking.hpp`, `hud_attribution.hpp`,
  `hud_results.hpp`, `rules.hpp`, `observation.hpp`, `devlog.hpp`, `memory_policy.hpp`, `speech_leveler.hpp`.
- Windows-only: `common.hpp`, `detector.hpp`, `engine.hpp`, `media.hpp`, `gpu.hpp`, `audio.hpp`,
  `mic_denoiser.hpp`, `selftest.hpp`, `main.cpp`, and all C#/XAML. Keep Windows includes out of portable headers.
- `config/` contains rule settings and HUD glyph templates; `tests/` contains native scenarios and smoke scripts.
- Read the relevant project documentation before work; preserve existing uncommitted changes.

## Code conventions
- Native code is dense: long lines, few blank lines, comments only to explain why. Do not reformat untouched code.
- Headers use `#pragma once`; never replace it with include guards.
- Do not pad aggregate initializers with empty fields just to silence warnings; portable tests use `-Wall`, not `-Wextra`.
- Do not use macros for field lists; use nlohmann JSON initializer lists, as in `clipJson`.
- Reuse values already available to another thread; do not construct expensive objects such as `LocalOcr` just to read them.
- Never shadow a variable in the same function. Check exact signatures, includes, captures and object lifetimes.
- UI strings are Simplified Chinese; JSON keys are English; developer-log `reason`/`detail` text is Chinese.
- WPF text buttons carry an `IconText` icon, as in "打开文件夹" and "重试保存".
- Settings rows use `Grid Style="{StaticResource Row}"`, a label with an explanatory `ToolTip`, and controls on the right.
- Empty optional TextBlocks collapse through a `Text=""` trigger.
- Synchronize shared state across pipe-reader, monitor, capture/analyze and export threads.
- Developer mode only records diagnostics; it must not change which clips are saved.
- Manual exports release packet references as they are written and must not stop capture due to held export references.
- Keep automatic-export retry behavior; failed manual exports are not retryable.

## Build and verification
Run portable tests from the repository root on macOS (Apple clang, C++20):
```sh
mkdir -p build
clang++ -std=c++20 -O2 -Wall tests/rules_tests.cpp -o build/rules_tests && build/rules_tests
clang++ -std=c++20 -O2 -Wall tests/hud_tests.cpp -o build/hud_tests && build/hud_tests
clang++ -std=c++20 -O2 -Wall tests/speech_tests.cpp -o build/speech_tests && build/speech_tests
for h in image names digits hud_tracking hud_attribution hud_results rules observation devlog memory_policy speech_leveler; do
  clang++ -std=c++20 -fsyntax-only -Wall -Wno-pragma-once-outside-header -x c++ src/native/$h.hpp || exit 1
done
```
- On Windows, run `./scripts/build.ps1` to compile the worker/WPF app and run all C++ tests.
- Windows-only code cannot compile on macOS; inspect its diff carefully and report what remains unverified.
- When the brief authorizes a branch push, push it and let `.github/workflows/build.yml` run on the Windows runner.
- Build runs on every branch push, builds/tests the package, and uploads the package and UI screenshots.
- Find the run for the pushed commit; use `gh run watch <id> --exit-status` and inspect failed steps.
- Fix failures within the authorized scope, push again, and repeat until green; do not claim hosted CI proves real GPU/game capture.

## Scope and local files
- Follow the brief's authorization boundaries; do not commit, push, publish or create tags unless authorized.
- Never commit `artifacts/`: it is gitignored local material, including fixtures and frames from YouTube videos.
- Never commit build output (`build/`, `dist/`, `bin/`, `obj/`) or local toolchains (`.tools/`).
- Keep temporary scripts/intermediates in a task subdirectory; clean up only files you created, retaining reusable scripts.
- Do not delete existing files or introduce unrelated dependencies or changes.
- Keep Mihomo available; never disable its kernel/TUN without a confirmed independent timed restore.

## Release and documentation
- Pushing a `v*` tag runs `.github/workflows/release.yml` and publishes a GitHub Release.
- Release notes belong in `.github/release-notes/<tag>.md`; the first line is `# title`.
- The version lives in `src/ApexPerif.App/ApexPerif.App.csproj` and the default in `scripts/package.ps1`.
- Only create release tags or edit release notes when the brief explicitly asks; ordinary branch pushes are builds, not releases.
- `README.md`, `docs/VALIDATION.md` and release notes are user-facing Chinese documents written by Claude.
- Do not edit those documents unless asked; put the facts Claude needs in your report instead.
- Finish with result, files changed, verification evidence, assumptions and open issues; state which Windows checks ran.
