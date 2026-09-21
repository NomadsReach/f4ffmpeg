import argparse
from pathlib import Path
import re
import subprocess
import tempfile


PREAMBLE = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <filesystem>
#include <format>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include "playbackPolicy.h"
#ifdef TEST_REAL_REX
#include <REX/LOG.h>
namespace REX::Impl {
void Log(std::source_location, REX::ELogLevel, std::string_view) {}
void Log(std::source_location, REX::ELogLevel, std::wstring_view) {}
}
#else
namespace REX {
template<class... T> struct DEBUG {
    explicit DEBUG(std::format_string<T...> f, T&&... v) {
        (void)std::format(f, std::forward<T>(v)...);
    }
};
template<class... T> DEBUG(std::format_string<T...>, T&&...) -> DEBUG<T...>;
}
#endif
using namespace f4ffmpeg;
std::string lowercasePath(std::string value) {
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}
void check(bool value, const char* label) {
    if (!value) throw std::runtime_error(label);
    std::cout << "PASS " << label << '\n';
}
enum class videoTargetMode { vanillaOverride, directTextureSwap };
'''

LOCATION_TESTS = r'''
int main() {
    videoPlaybackSettings base;
    base.looping = true;
    base.shuffle = true;
    base.playlist = {"global.mp4"};
    base.transition = transitionMethod::holdLastFrame;
    base.transitionImage = "global.png";
    locationPlaybackSettings loc;
    loc.locationEditorId = "Town";
    loc.looping = false;
    loc.shuffle = false;
    loc.playlist = {"local.mp4"};
    loc.hasPlaylist = true;
    loc.transition = transitionMethod::image;
    loc.transitionImage = "resolved/local.png";
    base.locationOverrides = {loc};
    auto r = resolveLocationPlaybackSettings(base, "TOWN");
    check(r.locationKey == "town", "canonical location identity");
    check(!r.settings.looping, "explicit false loop override");
    check(!r.settings.shuffle, "explicit false shuffle override");
    check(r.settings.transition == transitionMethod::image, "typed transition override");
    check(r.settings.transitionImage == "resolved/local.png", "resolved image path retained");
    check(r.settings.playlist == std::vector<std::string>{"global.mp4", "local.mp4"}, "add playlist order");
    check(r.settings.locationOverrides.empty() && base.locationOverrides.size() == 1, "result metadata cleared without mutating input");
    base.locationOverrides[0].overridePlaylist = true;
    r = resolveLocationPlaybackSettings(base, "town");
    check(r.overridePlaylist && r.settings.playlist == std::vector<std::string>{"local.mp4"}, "override replaces playlist");
    base.locationOverrides[0].playlist.clear();
    r = resolveLocationPlaybackSettings(base, "Town");
    check(r.settings.playlist.empty(), "explicit empty override clears playlist");
    base.locationOverrides[0].hasPlaylist = false;
    base.locationOverrides[0].looping.reset();
    base.locationOverrides[0].shuffle.reset();
    base.locationOverrides[0].transition.reset();
    base.locationOverrides[0].transitionImage.reset();
    r = resolveLocationPlaybackSettings(base, "Town");
    check(r.settings.looping && r.settings.shuffle && r.settings.playlist == base.playlist, "unspecified fields inherit");
    check(r.settings.transition == base.transition && r.settings.transitionImage == base.transitionImage, "unspecified transitions inherit");
    for (auto id : {static_cast<const char*>(nullptr), "", "Unknown"}) {
        r = resolveLocationPlaybackSettings(base, id);
        check(r.locationKey.empty() && r.settings.playlist == base.playlist, "unmatched editor retains global settings");
    }
}
'''

TARGET_TESTS = r'''
int main() {
    indexedVideoReplacement item;
    item.videoPath = "direct.mp4";
    item.playbackKey = "definition.ini";
    replacementIndex["screen.dds"] = item;
    auto r = resolveVideoTarget("screen.dds");
    check(r && r->videoPath == "direct.mp4", "direct video fallback preserved");
    check(r->playbackKey == "definition.ini", "base playback identity preserved");
    check(!resolveVideoTarget(nullptr) && !resolveVideoTarget("") && !resolveVideoTarget("missing"), "invalid texture inputs rejected");
    replacementIndex["screen.dds"].standalonePlaylist = true;
    replacementIndex["screen.dds"].playbackSettings.playlist = {"global.mp4"};
    injected.settings.playlist = {"local.mp4"};
    injected.locationKey = "town";
    r = resolveVideoTarget("screen.dds");
    check(r && r->videoPath == "global.mp4", "standalone global priority unchanged");
    check(r->playbackKey == "definition.ini|location:town", "location playback identity preserved");
    replacementIndex["screen.dds"].standalonePlaylist = false;
    r = resolveVideoTarget("screen.dds");
    check(r && r->videoPath == "global.mp4", "sidecar global priority unchanged");
    replacementIndex["screen.dds"].playbackSettings.playlist.clear();
    replacementIndex["screen.dds"].hasGlobalPlayback = false;
    r = resolveVideoTarget("screen.dds");
    check(r && r->videoPath == "local.mp4", "matching location-only source retained");
    injected.locationKey.clear();
    check(!resolveVideoTarget("screen.dds"), "location-only source hidden outside match");
    injected.locationKey = "town";
    injected.settings.playlist.clear();
    check(!resolveVideoTarget("screen.dds"), "empty location-only source rejected");
}
'''


def between(text: str, first: str, last: str) -> str:
    start = text.index(first)
    return text[start:text.index(last, start)]


def programs(source: str) -> dict[str, str]:
    structs = between(source, "        struct indexedVideoReplacement", "        std::optional<resolvedVideoTarget>")
    location_struct = between(source, "        struct resolvedLocationPlaybackSettings", "        resolvedLocationPlaybackSettings resolveLocationPlaybackSettings(")
    location = between(source, "        resolvedLocationPlaybackSettings resolveLocationPlaybackSettings(", "            auto* player = RE::PlayerCharacter::GetSingleton();")
    location = location.replace("const videoPlaybackSettings& baseSettings)", "const videoPlaybackSettings& baseSettings, const char* editorId)", 1)
    location += "applyEditorId(editorId); return result; }\n"
    target = between(source, "        std::optional<resolvedVideoTarget>\n        resolveVideoTarget(\n", "        std::mutex targetRegistryMutex;")
    target_support = r'''
std::unordered_map<std::string, indexedVideoReplacement> replacementIndex;
resolvedLocationPlaybackSettings injected;
resolvedLocationPlaybackSettings resolveLocationPlaybackSettings(const videoPlaybackSettings&) { return injected; }
std::string canonicalTextureLookupPath(const char* path) { return path; }
'''
    statements = re.findall(r'REX::(?:LOG_)?DEBUG\s*\(.*?\);', source, re.S)
    selected = [s for s in statements if any(marker in s for marker in (
        "URL deferred:", "parsing INI item", "f4ffmpeg INDEXING standalone INI", "blocked render-pass after nulling base-texture"))]
    for marker in ("URL deferred:", "parsing INI item", "blocked render-pass after nulling base-texture"):
        if sum(marker in s for s in selected) != 1:
            raise ValueError(f"Expected one production log statement for {marker}")
    logging_support = r'''
const char* workshopTvEffectKindName(int) { return "scan"; }
int main() {
std::string entry = "entry", value = "value";
std::filesystem::path iniPath = "playlist.ini";
int lineNumber = 1, effectKind = 0;
void* shaderProperty = nullptr;
std::optional<std::string> relativeStem = "screen";
videoPlaybackSettings playbackSettings;
bool hasLocationPlayback = true, hasGlobalPlayback = true;
'''
    return {
        "location": PREAMBLE + location_struct + location + LOCATION_TESTS,
        "target": PREAMBLE + structs + location_struct + target_support + target + TARGET_TESTS,
        "logging": PREAMBLE + logging_support + "\n".join(selected) + '\ncheck(true, "production log statements compile and format");}\n',
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--compiler", default="c++")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--allow-fixture-logger", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    source = (root / "src/nifHandler.cpp").read_text(encoding="utf-8-sig")
    rex = root / "lib/commonlibf4/lib/commonlib-shared/include"
    real_rex = (rex / "REX/LOG.h").is_file()
    if not real_rex and not args.allow_fixture_logger:
        raise FileNotFoundError("Pinned REX/LOG.h is missing; initialize submodules")
    print("Logger contract: " + ("pinned REX/LOG.h with standard-only BASE shim" if real_rex else "local API-shape fixture"))
    code = programs(source)
    failures = []
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        (tmp / "REX").mkdir()
        (tmp / "REX/BASE.h").write_text("#pragma once\n#include <format>\n#include <source_location>\n#include <string_view>\n#include <utility>\n", encoding="utf-8")
        for name, text in code.items():
            cpp, exe = tmp / f"{name}.cpp", tmp / f"{name}.exe"
            cpp.write_text(text, encoding="utf-8")
            includes = [tmp, root / "src"] + ([rex] if real_rex else [])
            if Path(args.compiler).stem.lower() == "cl":
                cmd = [args.compiler, "/nologo", "/std:c++latest", "/EHsc", "/utf-8", str(cpp), f"/Fe:{exe}"]
                cmd += [f"/I{path}" for path in includes] + (["/DTEST_REAL_REX=1"] if real_rex else [])
            else:
                cmd = [args.compiler, "-std=c++23", str(cpp), "-o", str(exe)]
                cmd += [f"-I{path}" for path in includes] + (["-DTEST_REAL_REX=1"] if real_rex else [])
            compiled = subprocess.run(cmd, cwd=tmp, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            output = compiled.stdout
            result = compiled.returncode
            if result == 0:
                ran = subprocess.run([str(exe)], cwd=tmp, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                result, output = ran.returncode, output + ran.stdout
            print(f"{name}: exit={result}\n{output}")
            if args.output:
                args.output.mkdir(parents=True, exist_ok=True)
                (args.output / f"{name}.cpp").write_text(text, encoding="utf-8")
                (args.output / f"{name}.log").write_text(output, encoding="utf-8")
            if result:
                failures.append(name)
    print(f"GROUPS: {len(code)-len(failures)}/{len(code)} passed")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
