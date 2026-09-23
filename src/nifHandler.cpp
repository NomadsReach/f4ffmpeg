#include "pch.h"
#include "nifHandler.h"
#include "urlHandler.h"
#include "manager.h"
#include "config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <RE/B/BSGraphics.h>
#include <RE/B/BSGeometry.h>
#include <RE/B/BSShaderMaterial.h>
#include <RE/B/BSShaderProperty.h>
#include <RE/B/BSShaderTextureSet.h>
#include <RE/B/BSTextureSet.h>
#include <RE/T/TESCellFullyLoadedEvent.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESBoundObject.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESWorldSpace.h>
#include <RE/N/NEW_REFR_DATA.h>
#include <RE/N/NiAVObject.h>
#include <RE/N/NiNode.h>
#include <RE/N/NiPointer.h>
#include <RE/N/NiProperty.h>
#include <RE/N/NiTexture.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/B/BGSLocation.h>
#include <REL/Relocation.h>
#include <REX/FModule.h>
#include <REX/W32/D3D11.h>
#include <REX/W32/KERNEL32.h>

namespace f4ffmpeg
{
    namespace
    {
        constexpr std::string_view dataPrefix =
            "data\\";

        constexpr std::string_view texturesPrefix =
            "textures\\";

        constexpr std::string_view directSwapSuffix =
            "_video.dds";

        constexpr std::string_view ddsSuffix =
            ".dds";

        constexpr std::string_view workshopTvRasterScanTexture =
            "textures\\effects\\tvanim\\rasterscananim_d.dds";

        constexpr std::string_view nukaColaCommercialScreenTexture =
            "textures\\actors\\dlc04\\nukatron\\nukaandcappycommercial01_d.dds";


        // Keep .mov first for backward-compatible collision precedence.
        // The decoder itself is FFmpeg-backed; nifHandler only needs to avoid
        // treating unrelated Data\\Video sidecars/config files as video inputs.
        constexpr std::array<std::string_view, 13>
            supportedVideoExtensions{
                ".mov",
                ".mp4",
                ".m4v",
                ".mkv",
                ".webm",
                ".avi",
                ".wmv",
                ".mpg",
                ".mpeg",
                ".ts",
                ".m2ts",
                ".ogv",
                ""
            };

        // Playlist transition images are pre-decoded once when that manager is lazily activated.
        // Deliberately exclude video containers: Image is a static decoder-gap
        // fallback, not authored transition media.
        constexpr std::array<std::string_view, 7>
            supportedTransitionImageExtensions{
                ".dds",
                ".png",
                ".jpg",
                ".jpeg",
                ".bmp",
                ".tga",
                ".webp"
            };

        constexpr std::size_t psSetShaderResourcesVtableIndex =
            8;

        // BSEffectShaderProperty overrides both of these BSShaderProperty
        // virtuals. GetRenderPasses is the reliable draw-time boundary; the
        // original GetBaseTexture slot gives us Bethesda's exact source NiTexture.
        constexpr std::size_t effectGetRenderPassesVtableIndex =
            0x2B;

        constexpr std::size_t effectGetBaseTextureVtableIndex =
            0x39;

        char asciiLower(char value)
        {
            if (
                value >= 'A' &&
                value <= 'Z')
            {
                return static_cast<char>(
                    value - 'A' + 'a'
                );
            }

            return value;
        }

        bool startsWithInsensitive(
            std::string_view value,
            std::string_view prefix)
        {
            if (value.size() < prefix.size())
                return false;

            for (
                std::size_t index = 0;
                index < prefix.size();
                ++index)
            {
                if (
                    asciiLower(value[index]) !=
                    asciiLower(prefix[index]))
                {
                    return false;
                }
            }

            return true;
        }

        bool endsWithInsensitive(
            std::string_view value,
            std::string_view suffix)
        {
            if (value.size() < suffix.size())
                return false;

            const auto offset =
                value.size() - suffix.size();

            for (
                std::size_t index = 0;
                index < suffix.size();
                ++index)
            {
                if (
                    asciiLower(value[offset + index]) !=
                    asciiLower(suffix[index]))
                {
                    return false;
                }
            }

            return true;
        }

        int videoExtensionPriority(
            std::string_view extension)
        {
            for (
                std::size_t index = 0;
                index < supportedVideoExtensions.size();
                ++index)
            {
                if (endsWithInsensitive(
                        extension,
                        supportedVideoExtensions[index]))
                {
                    // Earlier entries have higher precedence. This makes .mov
                    // win stem collisions, preserving the behavior of releases
                    // that only recognized .mov.
                    return static_cast<int>(
                        supportedVideoExtensions.size() - index
                    );
                }
            }

            return -1;
        }

        // Network URIs (http/https) are valid streaming playlist entries.
        // They are resolved via yt-dlp at load time and handed to FFmpeg
        // verbatim; file-existence and extension checks do not apply.
        bool isSupportedVideoUrl(
            std::string_view entry)
        {
            return startsWithInsensitive(entry, "http://") ||
                   startsWithInsensitive(entry, "https://");
        }

        bool isSupportedVideoPath(
            const std::filesystem::path& path)
        {
            return videoExtensionPriority(
                       path.extension().string()
                   ) >= 0;
        }


        bool isSupportedTransitionImagePath(
            const std::filesystem::path& path)
        {
            const auto extension =
                path.extension().string();

            return std::any_of(
                supportedTransitionImageExtensions.begin(),
                supportedTransitionImageExtensions.end(),
                [&extension](std::string_view candidate)
                {
                    return endsWithInsensitive(
                        extension,
                        candidate
                    );
                }
            );
        }

        std::string lowercasePath(
            std::string_view path)
        {
            std::string result{path};

            std::transform(
                result.begin(),
                result.end(),
                result.begin(),
                [](char value)
                {
                    return asciiLower(value);
                }
            );

            return result;
        }

        std::string normalizeTexturePath(
            std::string_view texturePath)
        {
            std::string normalized{
                texturePath
            };

            std::replace(
                normalized.begin(),
                normalized.end(),
                '/',
                '\\'
            );

            while (
                !normalized.empty() &&
                normalized.front() == '\\')
            {
                normalized.erase(
                    normalized.begin()
                );
            }

            if (startsWithInsensitive(
                    normalized,
                    dataPrefix))
            {
                normalized.erase(
                    0,
                    dataPrefix.size()
                );
            }

            return normalized;
        }

        // Bethesda returns texture names in both "Textures\..." and root-relative
        // forms (for example "Effects\TVAnim\..."). Keep one canonical lookup
        // namespace so BGSM/texture-set and BGEM/effect paths meet the same index.
        std::string canonicalTextureLookupPath(
            std::string_view texturePath)
        {
            std::string canonical =
                normalizeTexturePath(
                    texturePath
                );

            if (canonical.empty())
                return canonical;

            if (!startsWithInsensitive(
                    canonical,
                    texturesPrefix))
            {
                canonical.insert(
                    0,
                    texturesPrefix
                );
            }

            // NiTexture names are normally explicit DDS paths, but tolerate the
            // extensionless form because the engine's texture APIs do as well.
            const auto lastSlash =
                canonical.find_last_of('\\');

            const auto lastDot =
                canonical.find_last_of('.');

            if (
                lastDot == std::string::npos ||
                (lastSlash != std::string::npos &&
                 lastDot < lastSlash))
            {
                canonical += ddsSuffix;
            }

            return canonical;
        }


        std::string trimIniValue(
            std::string_view value)
        {
            std::size_t begin = 0;
            std::size_t end = value.size();

            while (
                begin < end &&
                (value[begin] == ' ' ||
                 value[begin] == '\t' ||
                 value[begin] == '\r' ||
                 value[begin] == '\n'))
            {
                ++begin;
            }

            while (
                end > begin &&
                (value[end - 1] == ' ' ||
                 value[end - 1] == '\t' ||
                 value[end - 1] == '\r' ||
                 value[end - 1] == '\n'))
            {
                --end;
            }

            std::string result{
                value.substr(
                    begin,
                    end - begin
                )
            };

            if (
                result.size() >= 2 &&
                ((result.front() == '"' &&
                  result.back() == '"') ||
                 (result.front() == '\'' &&
                  result.back() == '\'')))
            {
                result = result.substr(
                    1,
                    result.size() - 2
                );
            }

            return result;
        }

        std::optional<bool> parseIniBool(
            std::string_view value)
        {
            const std::string lowered =
                lowercasePath(
                    trimIniValue(value)
                );

            if (
                lowered == "1" ||
                lowered == "true" ||
                lowered == "yes" ||
                lowered == "on")
            {
                return true;
            }

            if (
                lowered == "0" ||
                lowered == "false" ||
                lowered == "no" ||
                lowered == "off")
            {
                return false;
            }

            return std::nullopt;
        }

        std::optional<RE::TESFormID> parseConfiguredFormId(
            std::string_view value)
        {
            if (startsWithInsensitive(value, "0x"))
                value.remove_prefix(2);

            // Editor IDs can be freely named. Treat only an exact eight-digit
            // hexadecimal value as a FormID selector.
            if (value.size() != 8)
                return std::nullopt;

            RE::TESFormID formId = 0;
            const auto [end, error] = std::from_chars(
                value.data(),
                value.data() + value.size(),
                formId,
                16
            );

            if (error != std::errc{} || end != value.data() + value.size())
                return std::nullopt;

            return formId;
        }

        bool resemblesFormIdSelector(std::string_view value)
        {
            if (startsWithInsensitive(value, "0x"))
                return true;

            return !value.empty() && std::all_of(
                value.begin(),
                value.end(),
                [](char character)
                {
                    return (character >= '0' && character <= '9') ||
                        (character >= 'A' && character <= 'F') ||
                        (character >= 'a' && character <= 'f');
                }
            );
        }

        struct parsedLocationSection
        {
            std::string editorId;
            std::string subsection;
        };

        std::optional<parsedLocationSection> parseLocationSection(
            std::string_view section)
        {
            constexpr std::string_view prefix = "location.";

            if (!startsWithInsensitive(section, prefix))
                return std::nullopt;

            const std::string_view remainder =
                section.substr(prefix.size());
            const auto separator = remainder.find('.');
            const std::string_view editorId =
                remainder.substr(0, separator);

            if (editorId.empty())
                return std::nullopt;

            return parsedLocationSection{
                std::string{editorId},
                separator == std::string_view::npos
                    ? std::string{}
                    : std::string{remainder.substr(separator + 1)}
            };
        }

        locationPlaybackSettings* getLocationOverride(
            videoPlaybackSettings& settings,
            std::string_view editorId)
        {
            const std::string normalized = lowercasePath(editorId);

            for (auto& overrideSettings : settings.locationOverrides)
            {
                if (lowercasePath(overrideSettings.locationEditorId) == normalized)
                    return std::addressof(overrideSettings);
            }

            settings.locationOverrides.emplace_back();
            auto& overrideSettings = settings.locationOverrides.back();
            overrideSettings.locationEditorId = std::string{editorId};
            return std::addressof(overrideSettings);
        }

        void appendPlaylistEntry(
            videoPlaybackSettings& settings,
            const std::filesystem::path& iniPath,
            std::string_view value)
        {
            std::string entry =
                trimIniValue(value);

            if (entry.empty())
                return;

            // Network entries are retained verbatim. Resolution is deferred
            // to decoder activation (see decoder::open): yt-dlp's direct media
            // URLs are signed and expire, so resolving at INI/index load time
            // frequently hands FFmpeg an already-expired URL by the time the
            // lazy manager actually starts playback.
            if (isSupportedVideoUrl(entry))
            {
                settings.playlist.emplace_back(entry);
                REX::DEBUG("URL deferred: {}", entry);
                return;
            }

            std::filesystem::path entryPath{
                entry
            };

            if (entryPath.is_relative())
            {
                entryPath =
                    iniPath.parent_path() /
                    entryPath;
            }

            settings.playlist.emplace_back(
                entryPath.lexically_normal().string()
            );
        }


        void setPlaylistTransitionImage(
            videoPlaybackSettings& settings,
            const std::filesystem::path& iniPath,
            std::string_view value,
            std::size_t lineNumber)
        {
            std::string entry =
                trimIniValue(value);

            if (entry.empty())
                return;

            if (startsWithInsensitive(entry, "http://") ||
                startsWithInsensitive(entry, "https://"))
            {
                settings.transitionImage = entry;
                return;
            }

            std::filesystem::path imagePath{
                entry
            };

            if (imagePath.is_relative())
            {
                imagePath =
                    iniPath.parent_path() /
                    imagePath;
            }

            imagePath = imagePath.lexically_normal();

            if (!isSupportedTransitionImagePath(imagePath))
            {
                REX::WARN(
                    "f4ffmpeg ignored non-image Transition.Image '{}' on line {} of '{}'.",
                    entry,
                    lineNumber,
                    iniPath.string()
                );
                return;
            }

            settings.transitionImage =
                imagePath.string();
        }

        videoPlaybackSettings loadPlaybackSettingsFromIni(
            const std::filesystem::path& iniPath)
        {
            videoPlaybackSettings settings{};

            std::error_code statusError;

            const bool exists =
                std::filesystem::exists(
                    iniPath,
                    statusError
                );

            if (statusError)
            {
                REX::WARN(
                    "f4ffmpeg could not inspect playback INI '{}': {}.",
                    iniPath.string(),
                    statusError.message()
                );

                return settings;
            }

            if (!exists)
                return settings;

            const bool regularFile =
                std::filesystem::is_regular_file(
                    iniPath,
                    statusError
                );

            if (statusError)
            {
                REX::WARN(
                    "f4ffmpeg could not inspect playback INI '{}': {}.",
                    iniPath.string(),
                    statusError.message()
                );

                return settings;
            }

            if (!regularFile)
                return settings;

            std::ifstream input(
                iniPath,
                std::ios::in |
                    std::ios::binary
            );

            if (!input)
            {
                REX::WARN(
                    "f4ffmpeg could not open playback INI '{}'; using defaults.",
                    iniPath.string()
                );

                return settings;
            }

            std::string section;
            std::string line;
            std::size_t lineNumber = 0;

            while (std::getline(input, line))
            {
                ++lineNumber;

                if (
                    lineNumber == 1 &&
                    line.size() >= 3 &&
                    static_cast<unsigned char>(line[0]) == 0xEF &&
                    static_cast<unsigned char>(line[1]) == 0xBB &&
                    static_cast<unsigned char>(line[2]) == 0xBF)
                {
                    line.erase(0, 3);
                }

                std::string trimmed =
                    trimIniValue(line);

                if (
                    trimmed.empty() ||
                    trimmed.front() == ';' ||
                    trimmed.front() == '#')
                {
                    continue;
                }

                if (
                    trimmed.size() >= 2 &&
                    trimmed.front() == '[' &&
                    trimmed.back() == ']')
                {
                    section =
                        lowercasePath(
                            trimIniValue(
                                std::string_view{trimmed}.substr(
                                    1,
                                    trimmed.size() - 2
                                )
                            )
                        );

                    continue;
                }

                const auto equals =
                    trimmed.find('=');

                if (equals == std::string::npos)
                {
                    REX::WARN(
                        "f4ffmpeg ignored malformed playback INI line {} in '{}'.",
                        lineNumber,
                        iniPath.string()
                    );

                    continue;
                }

                const std::string key =
                    lowercasePath(
                        trimIniValue(
                            std::string_view{trimmed}.substr(
                                0,
                                equals
                            )
                        )
                    );

                const std::string value =
                    trimIniValue(
                        std::string_view{trimmed}.substr(
                            equals + 1
                        )
                    );

                if (const auto locationSection =
                        parseLocationSection(section))
                {
                    auto* locationSettings = getLocationOverride(
                        settings,
                        locationSection->editorId
                    );

                    const bool locationPlaybackSection =
                        locationSection->subsection.empty() ||
                        locationSection->subsection == "playback";
                    const bool locationTransitionSection =
                        locationSection->subsection == "transition";
                    const bool locationPlaylistSection =
                        locationSection->subsection == "playlist";

                    if (locationPlaybackSection && key == "mode")
                    {
                        const std::string mode = lowercasePath(value);

                        if (mode == "add")
                            locationSettings->overridePlaylist = false;
                        else if (mode == "override")
                            locationSettings->overridePlaylist = true;
                        else
                            REX::WARN(
                                "f4ffmpeg ignored invalid location Mode '{}' on line {} of '{}'.",
                                value,
                                lineNumber,
                                iniPath.string()
                            );

                        continue;
                    }

                    if (locationPlaybackSection &&
                        (key == "loop" || key == "looping"))
                    {
                        const auto parsed = parseIniBool(value);
                        if (parsed)
                            locationSettings->looping = *parsed;
                        else
                            REX::WARN(
                                "f4ffmpeg ignored invalid location Loop value '{}' on line {} of '{}'.",
                                value,
                                lineNumber,
                                iniPath.string()
                            );
                        continue;
                    }

                    if (locationPlaybackSection && key == "shuffle")
                    {
                        const auto parsed = parseIniBool(value);
                        if (parsed)
                            locationSettings->shuffle = *parsed;
                        else
                            REX::WARN(
                                "f4ffmpeg ignored invalid location Shuffle value '{}' on line {} of '{}'.",
                                value,
                                lineNumber,
                                iniPath.string()
                            );
                        continue;
                    }

                    if ((locationTransitionSection &&
                         (key == "method" || key == "mode")) ||
                        (locationPlaybackSection && key == "transitionmethod"))
                    {
                        const auto parsed = parseTransitionMethod(value, true);
                        if (parsed)
                            locationSettings->transition = *parsed;
                        else
                            REX::WARN(
                                "f4ffmpeg ignored invalid location transition method '{}' on line {} of '{}'.",
                                value,
                                lineNumber,
                                iniPath.string()
                            );
                        continue;
                    }

                    if ((locationTransitionSection &&
                         (key == "image" || key == "file")) ||
                        (locationPlaybackSection && key == "transitionimage"))
                    {
                        videoPlaybackSettings temporary{};
                        setPlaylistTransitionImage(
                            temporary,
                            iniPath,
                            value,
                            lineNumber
                        );
                        locationSettings->transitionImage =
                            std::move(temporary.transitionImage);
                        continue;
                    }

                    if ((locationPlaybackSection &&
                         (key == "playlist" || key == "playlistitem")) ||
                        (locationPlaylistSection &&
                         (key == "item" || key == "entry" || key == "file")))
                    {
                        videoPlaybackSettings temporary{};
                        appendPlaylistEntry(temporary, iniPath, value);
                        if (!temporary.playlist.empty())
                        {
                            locationSettings->playlist.emplace_back(
                                std::move(temporary.playlist.front())
                            );
                            locationSettings->hasPlaylist = true;
                        }
                        continue;
                    }

                    REX::WARN(
                        "f4ffmpeg ignored unknown location playback INI setting '{}={}' in section [{}] of '{}'.",
                        key,
                        value,
                        section,
                        iniPath.string()
                    );
                    continue;
                }

                const bool playbackSection =
                    section.empty() ||
                    section == "playback";

                if (
                    playbackSection &&
                    (key == "loop" ||
                     key == "looping"))
                {
                    const auto parsed =
                        parseIniBool(value);

                    if (parsed)
                    {
                        settings.looping = *parsed;
                    }
                    else
                    {
                        REX::WARN(
                            "f4ffmpeg ignored invalid Loop value '{}' on line {} of '{}'.",
                            value,
                            lineNumber,
                            iniPath.string()
                        );
                    }

                    continue;
                }

                if (
                    playbackSection &&
                    key == "shuffle")
                {
                    const auto parsed =
                        parseIniBool(value);

                    if (parsed)
                    {
                        settings.shuffle = *parsed;
                    }
                    else
                    {
                        REX::WARN(
                            "f4ffmpeg ignored invalid Shuffle value '{}' on line {} of '{}'.",
                            value,
                            lineNumber,
                            iniPath.string()
                        );
                    }

                    continue;
                }

                const bool transitionSection =
                    section == "transition";

                if (
                    (transitionSection &&
                     (key == "method" ||
                      key == "mode")) ||
                    (playbackSection &&
                     key == "transitionmethod"))
                {
                    const auto parsed =
                        parseTransitionMethod(
                            value,
                            true
                        );

                    if (parsed)
                    {
                        settings.transition = *parsed;
                    }
                    else
                    {
                        REX::WARN(
                            "f4ffmpeg ignored invalid playlist transition method '{}' on line {} of '{}'.",
                            value,
                            lineNumber,
                            iniPath.string()
                        );
                    }

                    continue;
                }

                if (
                    (transitionSection &&
                     (key == "image" ||
                      key == "file")) ||
                    (playbackSection &&
                     key == "transitionimage"))
                {
                    setPlaylistTransitionImage(
                        settings,
                        iniPath,
                        value,
                        lineNumber
                    );

                    continue;
                }

                if (
                    (playbackSection &&
                     (key == "playlist" ||
                      key == "playlistitem")) ||
                    (section == "playlist" &&
                     (key == "item" ||
                      key == "entry" ||
                      key == "file")))
                {
                    REX::DEBUG(
                        "  [INI {} line {}]: parsing INI item \"{}\"",
                        iniPath.string(), lineNumber, value
                    );
                    appendPlaylistEntry(
                        settings,
                        iniPath,
                        value
                    );

                    continue;
                }

                REX::WARN(
                    "f4ffmpeg ignored unknown playback INI setting '{}={}' in section [{}] of '{}'.",
                    key,
                    value,
                    section.empty()
                        ? "Playback"
                        : section,
                    iniPath.string()
                );
            }

            std::vector<std::string> validatedPlaylist;
            validatedPlaylist.reserve(
                settings.playlist.size()
            );

            for (const auto& entry : settings.playlist)
            {
                if (startsWithInsensitive(entry, "http://") ||
                    startsWithInsensitive(entry, "https://"))
                {
                    validatedPlaylist.emplace_back(entry);
                    REX::DEBUG("URL Validated as {}", entry);
                    continue;
                }

                const std::filesystem::path entryPath{
                    entry
                };

                if (!isSupportedVideoPath(entryPath))
                {
                    REX::WARN(
                        "f4ffmpeg ignored unsupported playlist media '{}' from '{}'.",
                        entry,
                        iniPath.string()
                    );

                    continue;
                }

                std::error_code entryError;
                const bool entryExists =
                    std::filesystem::exists(
                        entryPath,
                        entryError
                    );

                if (entryError)
                {
                    REX::WARN(
                        "f4ffmpeg could not inspect playlist media '{}' from '{}'.",
                        entry,
                        iniPath.string()
                    );

                    continue;
                }

                if (!entryExists)
                {
                    REX::WARN(
                        "f4ffmpeg ignored missing playlist media '{}' from '{}'.",
                        entry,
                        iniPath.string()
                    );

                    continue;
                }

                const bool regularEntry =
                    std::filesystem::is_regular_file(
                        entryPath,
                        entryError
                    );

                if (entryError || !regularEntry)
                {
                    REX::WARN(
                        "f4ffmpeg ignored non-file playlist media '{}' from '{}'.",
                        entry,
                        iniPath.string()
                    );

                    continue;
                }

                validatedPlaylist.emplace_back(
                    entry
                );
            }

            settings.playlist =
                std::move(validatedPlaylist);

            for (auto& locationSettings : settings.locationOverrides)
            {
                std::vector<std::string> validatedLocationPlaylist;
                validatedLocationPlaylist.reserve(
                    locationSettings.playlist.size()
                );

                for (const auto& entry : locationSettings.playlist)
                {
                    if (isSupportedVideoUrl(entry))
                    {
                        validatedLocationPlaylist.emplace_back(entry);
                        continue;
                    }

                    const std::filesystem::path entryPath{entry};
                    std::error_code entryError;

                    if (!isSupportedVideoPath(entryPath) ||
                        !std::filesystem::is_regular_file(entryPath, entryError))
                    {
                        REX::WARN(
                            "f4ffmpeg ignored invalid location playlist media '{}' for location '{}' from '{}'.",
                            entry,
                            locationSettings.locationEditorId,
                            iniPath.string()
                        );
                        continue;
                    }

                    validatedLocationPlaylist.emplace_back(entry);
                }

                locationSettings.playlist =
                    std::move(validatedLocationPlaylist);

                if (locationSettings.transition == transitionMethod::image &&
                    !locationSettings.transitionImage)
                {
                    REX::WARN(
                        "f4ffmpeg location '{}' requested transition Method=Image without an Image in '{}'.",
                        locationSettings.locationEditorId,
                        iniPath.string()
                    );
                }
            }

            if (
                settings.transitionImage &&
                settings.transition != transitionMethod::image)
            {
                REX::WARN(
                    "f4ffmpeg playlist transition image '{}' is present but Method is not Image; the image will not be used.",
                    *settings.transitionImage
                );
            }

            if (
                settings.transition == transitionMethod::image &&
                !settings.transitionImage)
            {
                REX::WARN(
                    "f4ffmpeg playlist requested transition Method=Image without an Image; global fallback will be used."
                );
            }

            REX::INFO(
                "f4ffmpeg loaded playback INI '{}': loop={}, shuffle={}, playlist entries={}, transition={}, transition image={}.",
                iniPath.string(),
                settings.looping,
                settings.shuffle,
                settings.playlist.size(),
                settings.transition
                    ? transitionMethodName(*settings.transition)
                    : "<global>",
                settings.transitionImage
                    ? settings.transitionImage->c_str()
                    : "<none>"
            );

            return settings;
        }

        videoPlaybackSettings loadPlaybackSettingsForVideo(
            const std::filesystem::path& videoPath)
        {
            std::filesystem::path iniPath =
                videoPath;

            iniPath.replace_extension(".ini");

            return loadPlaybackSettingsFromIni(
                iniPath
            );
        }

        bool hasSupportedVideoSibling(
            const std::filesystem::path& iniPath)
        {
            std::error_code statusError;

            for (const auto extension :
                 supportedVideoExtensions)
            {
                std::filesystem::path candidate =
                    iniPath;

                candidate.replace_extension(
                    std::string{extension}
                );

                const bool exists =
                    std::filesystem::is_regular_file(
                        candidate,
                        statusError
                    );

                if (statusError)
                {
                    statusError.clear();
                    continue;
                }

                if (exists)
                    return true;
            }

            return false;
        }

        bool isWorkshopTvRasterScanTexture(
            std::string_view texturePath)
        {
            return lowercasePath(
                       canonicalTextureLookupPath(
                           texturePath
                       )
                   ) == workshopTvRasterScanTexture;
        }

        const char* modeName(
            videoTargetMode mode)
        {
            switch (mode)
            {
                case videoTargetMode::vanillaOverride:
                    return "vanilla override";

                case videoTargetMode::directTextureSwap:
                    return "direct texture swap";
            }

            return "unknown";
        }

        int modePriority(
            videoTargetMode mode)
        {
            switch (mode)
            {
                case videoTargetMode::vanillaOverride:
                    return 0;

                case videoTargetMode::directTextureSwap:
                    return 1;
            }

            return -1;
        }

        struct indexedVideoReplacement
        {
            videoTargetMode mode =
                videoTargetMode::vanillaOverride;

            // Initial decoder source. For a standalone playlist this is the
            // first playlist item, not the identity of the playback definition.
            std::string videoPath;

            // Stable manager identity. Video-backed replacements use the video
            // path; standalone playlists use the .ini path so a playlist may
            // start with a video that is also independently indexed elsewhere.
            std::string playbackKey;

            videoPlaybackSettings playbackSettings;
            bool standalonePlaylist = false;
            bool hasGlobalPlayback = true;
        };

        struct resolvedVideoTarget
        {
            videoTargetMode mode =
                videoTargetMode::vanillaOverride;

            std::string texturePath;
            std::string videoPath;
            std::string playbackKey;
            videoPlaybackSettings playbackSettings;
        };

        std::optional<resolvedVideoTarget>
        resolveVideoTarget(const char* texturePath);

        struct resolvedLocationPlaybackSettings
        {
            videoPlaybackSettings settings;
            std::string locationKey;
            bool overridePlaylist = false;  // whether location playlist replaces global
        };

        resolvedLocationPlaybackSettings resolveLocationPlaybackSettings(
            const videoPlaybackSettings& baseSettings)
        {
            resolvedLocationPlaybackSettings result{baseSettings, {}, false};
            result.settings.locationOverrides.clear();

            auto applyEditorId =
                [&](const char* editorId) -> bool
                {
                    if (editorId == nullptr || *editorId == '\0')
                        return false;

                    const std::string locationKey = lowercasePath(editorId);
                    const auto overrideIt = std::find_if(
                        baseSettings.locationOverrides.begin(),
                        baseSettings.locationOverrides.end(),
                        [&locationKey](const auto& candidate)
                        {
                            return lowercasePath(candidate.locationEditorId) ==
                                locationKey;
                        }
                    );

                    if (overrideIt == baseSettings.locationOverrides.end())
                        return false;

                    const auto& locationSettings = *overrideIt;
                    if (locationSettings.looping)
                        result.settings.looping = *locationSettings.looping;
                    if (locationSettings.shuffle)
                        result.settings.shuffle = *locationSettings.shuffle;
                    if (locationSettings.transition)
                        result.settings.transition = locationSettings.transition;
                    if (locationSettings.transitionImage)
                        result.settings.transitionImage = locationSettings.transitionImage;

                    if (locationSettings.hasPlaylist)
                    {
                        if (locationSettings.overridePlaylist)
                            result.settings.playlist = locationSettings.playlist;
                        else
                            result.settings.playlist.insert(
                                result.settings.playlist.end(),
                                locationSettings.playlist.begin(),
                                locationSettings.playlist.end()
                            );
                    }

                    result.overridePlaylist = locationSettings.overridePlaylist;
                    result.locationKey = locationKey;
                    return true;
                };

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player == nullptr)
                return result;

            // Resolve from most-specific to broadest scope.  This keeps the
            // existing BGSLocation hierarchy behavior while also allowing the
            // Location.<EditorID> syntax to target cells and worldspaces.
            if (auto* cell = player->GetParentCell();
                cell != nullptr && applyEditorId(cell->GetFormEditorID()))
            {
                return result;
            }

            for (auto* location = player->currentLocation;
                 location != nullptr;
                 location = location->parentLoc)
            {
                if (applyEditorId(location->GetFormEditorID()))
                    return result;
            }

            // PlayerCharacter exposes the currently cached exterior worldspace.
            // In particular this lets [Location.Commonwealth.*] match the WRLD
            // EditorID "Commonwealth", while [Location.CommonwealthLocation.*]
            // continues to match the vanilla parent BGSLocation.
            if (auto* worldspace = player->cachedWorldspace;
                worldspace != nullptr &&
                applyEditorId(worldspace->GetFormEditorID()))
            {
                return result;
            }

            return result;
        }

        // Published once before hooks are installed, then read-only.
        std::unordered_map<
            std::string,
            indexedVideoReplacement>
            replacementIndex;

        std::mutex playbackRegistryMutex;

        std::unordered_set<std::string> nukaColaMachineScreenTargets;
        std::unordered_set<RE::TESFormID> nukaColaMachineScreenTargetFormIds;
        std::unordered_set<RE::TESFormID> injectedNukaColaMachineReferences;
        std::mutex injectedNukaColaMachineReferencesMutex;
        bool nukaColaMachineScreenInjectionEnabled = false;
        std::string nukaColaMachineScreenSourceForm;
        RE::TESBoundObject* nukaColaMachineScreenSource = nullptr;
        std::mutex nukaColaMachineScreenSourceMutex;
        std::atomic_bool nukaColaMachineCellEventObserved = false;

        std::unordered_map<
            std::string,
            std::shared_ptr<manager>>
            playbackByIdentity;

        enum class managerActivationState : std::uint8_t
        {
            dormant,
            queued,
            activating,
            active,
            failed
        };

        struct managerActivationRecord
        {
            std::string playbackKey;
            std::string videoPath;
            videoPlaybackSettings playbackSettings;
            std::atomic<managerActivationState> state =
                managerActivationState::dormant;
        };

        std::mutex activationRegistryMutex;
        std::unordered_map<
            std::string,
            std::shared_ptr<managerActivationRecord>>
            activationByIdentity;

        std::mutex activationQueueMutex;
        std::condition_variable activationQueueCv;
        std::deque<std::shared_ptr<managerActivationRecord>>
            activationQueue;
        std::thread activationWorker;
        bool activationEnabled = false;
        bool activationStopRequested = false;

        void requestPlaybackActivation(
            std::string_view playbackKey);
        void ensurePlaybackActivationRecord(
            std::string_view playbackKey,
            std::string_view videoPath,
            const videoPlaybackSettings& playbackSettings);
        void activationWorkerMain();

        void registerIndexedReplacement(
            std::string textureKey,
            indexedVideoReplacement replacement)
        {
            textureKey =
                lowercasePath(
                    canonicalTextureLookupPath(
                        textureKey
                    )
                );

            const auto existing =
                replacementIndex.find(
                    textureKey
                );

            if (existing == replacementIndex.end())
            {
                replacementIndex.emplace(
                    std::move(textureKey),
                    std::move(replacement)
                );

                return;
            }

            const int replacementModePriority =
                modePriority(replacement.mode);

            const int existingModePriority =
                modePriority(existing->second.mode);

            const int replacementExtensionPriority =
                videoExtensionPriority(
                    std::filesystem::path(
                        replacement.videoPath
                    ).extension().string()
                );

            const int existingExtensionPriority =
                videoExtensionPriority(
                    std::filesystem::path(
                        existing->second.videoPath
                    ).extension().string()
                );

            const bool replacementWins =
                replacementModePriority >
                    existingModePriority ||
                (
                    replacementModePriority ==
                        existingModePriority &&
                    replacementExtensionPriority >
                        existingExtensionPriority
                );

            if (replacementWins)
            {
                REX::WARN(
                    "f4ffmpeg replacement collision for '{}': "
                    "'{}' [{}] supersedes '{}' [{}].",
                    textureKey,
                    replacement.videoPath,
                    modeName(replacement.mode),
                    existing->second.videoPath,
                    modeName(existing->second.mode)
                );

                existing->second =
                    std::move(replacement);

                return;
            }

            REX::WARN(
                "f4ffmpeg replacement collision for '{}': keeping '{}' [{}], "
                "ignoring '{}' [{}].",
                textureKey,
                existing->second.videoPath,
                modeName(existing->second.mode),
                replacement.videoPath,
                modeName(replacement.mode)
            );
        }

        std::optional<std::filesystem::path>
        getGameRootPath()
        {
            std::vector<wchar_t> buffer(1024);

            while (true)
            {
                const auto length =
                    REX::W32::GetModuleFileNameW(
                        nullptr,
                        buffer.data(),
                        static_cast<std::uint32_t>(
                            buffer.size()
                        )
                    );

                if (length == 0)
                {
                    REX::WARN(
                        "f4ffmpeg could not resolve the Fallout executable path "
                        "while locating loose videos."
                    );

                    return std::nullopt;
                }

                if (length < buffer.size() - 1)
                {
                    std::filesystem::path executablePath(
                        buffer.data(),
                        buffer.data() + length
                    );

                    return executablePath.parent_path();
                }

                if (buffer.size() >= 32768)
                {
                    REX::WARN(
                        "f4ffmpeg executable path exceeded the supported length."
                    );

                    return std::nullopt;
                }

                buffer.resize(
                    buffer.size() * 2
                );
            }
        }

        bool isNukaColaCommercialVideoReplacementActive()
        {
            return resolveVideoTarget(
                nukaColaCommercialScreenTexture.data()
            ).has_value();
        }

        bool isNukaColaMachineScreenTarget(
            const RE::TESObjectREFR& reference)
        {
            if (nukaColaMachineScreenTargetFormIds.contains(
                    reference.GetFormID()))
            {
                return true;
            }

            const auto* baseObject = reference.GetObjectReference();
            if (baseObject != nullptr &&
                nukaColaMachineScreenTargetFormIds.contains(
                    baseObject->GetFormID()))
            {
                return true;
            }

            const char* editorId = baseObject != nullptr
                ? baseObject->GetFormEditorID()
                : nullptr;

            if (editorId != nullptr &&
                nukaColaMachineScreenTargets.contains(lowercasePath(editorId)))
            {
                return true;
            }

            const char* referenceEditorId = reference.GetFormEditorID();
            return referenceEditorId != nullptr &&
                nukaColaMachineScreenTargets.contains(
                    lowercasePath(referenceEditorId)
                );
        }

        class nukaColaMachineScreenSpawnData final :
            public RE::NEW_REFR_DATA
        {};

        RE::TESBoundObject* resolveNukaColaMachineScreenSourceForm()
        {
            RE::TESBoundObject* sourceForm = nullptr;
            {
                std::scoped_lock lock(nukaColaMachineScreenSourceMutex);
                sourceForm = nukaColaMachineScreenSource;

                if (sourceForm == nullptr)
                {
                    REX::INFO(
                        "f4ffmpeg resolving Nuka-Cola screen source form '{}'.",
                        nukaColaMachineScreenSourceForm
                    );

                    const RE::BSFixedString sourceEditorId{
                        nukaColaMachineScreenSourceForm.c_str()
                    };
                    sourceForm = RE::TESForm::GetFormByEditorID<
                        RE::TESBoundObject>(sourceEditorId);
                    nukaColaMachineScreenSource = sourceForm;

                    REX::INFO(
                        "f4ffmpeg Nuka-Cola screen source form '{}' resolution {}.",
                        nukaColaMachineScreenSourceForm,
                        sourceForm != nullptr ? "succeeded" : "failed"
                    );
                }
            }

            if (sourceForm == nullptr)
            {
                REX::WARN(
                    "f4ffmpeg could not resolve Nuka-World screen source form '{}'; disabling screen injection for this session.",
                    nukaColaMachineScreenSourceForm
                );
                nukaColaMachineScreenInjectionEnabled = false;
            }

            return sourceForm;
        }

        bool isNukaColaCommercialScreenReference(
            const RE::TESObjectREFR& reference)
        {
            const auto* baseObject = reference.GetObjectReference();
            if (baseObject == nullptr)
                return false;

            auto* sourceForm = resolveNukaColaMachineScreenSourceForm();
            return sourceForm != nullptr &&
                (baseObject == sourceForm ||
                 baseObject->GetFormID() == sourceForm->GetFormID());
        }

        constexpr float nukaColaScreenPositionTolerance = 16.0F;
        constexpr float nukaColaScreenAngleTolerance = 0.0872664626F;  // 5 degrees
        constexpr float twoPi = 6.28318530718F;

        float wrappedAngleDifference(float lhs, float rhs)
        {
            return std::fabs(std::remainder(lhs - rhs, twoPi));
        }

        bool nukaColaScreenMatchesTargetTransform(
            const RE::TESObjectREFR& screen,
            const RE::TESObjectREFR& target)
        {
            const auto screenPosition = screen.GetPosition();
            const auto targetPosition = target.GetPosition();
            const float dx = screenPosition.x - targetPosition.x;
            const float dy = screenPosition.y - targetPosition.y;
            const float dz = screenPosition.z - targetPosition.z;
            const float distanceSquared = dx * dx + dy * dy + dz * dz;

            if (distanceSquared >
                nukaColaScreenPositionTolerance *
                    nukaColaScreenPositionTolerance)
            {
                return false;
            }

            const auto& screenAngle = screen.data.angle;
            const auto& targetAngle = target.data.angle;
            return wrappedAngleDifference(screenAngle.x, targetAngle.x) <=
                       nukaColaScreenAngleTolerance &&
                wrappedAngleDifference(screenAngle.y, targetAngle.y) <=
                    nukaColaScreenAngleTolerance &&
                wrappedAngleDifference(screenAngle.z, targetAngle.z) <=
                    nukaColaScreenAngleTolerance;
        }

        bool injectNukaColaMachineScreen(
            RE::TESObjectREFR& reference)
        {
            auto* sourceForm = resolveNukaColaMachineScreenSourceForm();
            if (sourceForm == nullptr)
                return false;

            auto* cell = reference.GetParentCell();
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (cell == nullptr || dataHandler == nullptr)
            {
                REX::WARN(
                    "f4ffmpeg cannot spawn a Nuka-Cola commercial screen for reference {:08X}: {} is unavailable.",
                    reference.GetFormID(),
                    cell == nullptr ? "its parent cell" : "the data handler"
                );
                return false;
            }

            // This is the native equivalent of PlaceAtMe: the screen is a real
            // sibling reference, so Fallout owns its 3D and streaming lifecycle.
            nukaColaMachineScreenSpawnData spawn{};
            spawn.object = sourceForm;
            spawn.location = reference.GetPosition();
            spawn.direction = reference.data.angle;
            spawn.interior = cell->IsInterior() ? cell : nullptr;
            spawn.world = cell->IsExterior() ? cell->worldSpace : nullptr;
            spawn.forcePersist = false;

            const auto spawned = dataHandler->CreateReferenceAtLocation(spawn);
            if (!spawned)
            {
                REX::WARN(
                    "f4ffmpeg could not spawn Nuka-Cola commercial screen '{}' for reference {:08X}.",
                    nukaColaMachineScreenSourceForm,
                    reference.GetFormID()
                );
                return false;
            }

            REX::INFO(
                "f4ffmpeg spawned Nuka-Cola commercial screen '{}' for reference {:08X}.",
                nukaColaMachineScreenSourceForm,
                reference.GetFormID()
            );
            return true;
        }

        void injectNukaColaMachineScreensInCell(
            RE::TESObjectCELL& cell,
            const char* source)
        {
            if (!nukaColaMachineScreenInjectionEnabled)
                return;

            auto* sourceForm = resolveNukaColaMachineScreenSourceForm();
            if (sourceForm == nullptr)
                return;

            std::size_t referenceCount = 0;
            std::size_t targetCount = 0;
            std::size_t existingScreenCount = 0;
            std::size_t reusedCount = 0;
            std::size_t spawnedCount = 0;
            std::vector<RE::ObjectRefHandle> targets;
            std::vector<RE::ObjectRefHandle> existingScreens;

            // Inventory the cell before creating anything. Nuka-World already
            // places NukaColaMachineCommercialFxDLC04 references beside its own
            // vending machines; spawning another sibling at that same transform
            // produces the doubled-screen artifact this feature must avoid.
            cell.ForEachReference(
                [&](RE::TESObjectREFR* reference)
                {
                    ++referenceCount;

                    if (reference == nullptr)
                        return RE::BSContainer::ForEachResult::kContinue;

                    const auto* baseObject = reference->GetObjectReference();
                    if (baseObject == sourceForm ||
                        (baseObject != nullptr &&
                         baseObject->GetFormID() == sourceForm->GetFormID()))
                    {
                        ++existingScreenCount;
                        existingScreens.emplace_back(reference->GetHandle());
                    }

                    if (isNukaColaMachineScreenTarget(*reference))
                    {
                        ++targetCount;
                        targets.emplace_back(reference->GetHandle());
                    }

                    return RE::BSContainer::ForEachResult::kContinue;
                }
            );

            std::vector<bool> existingScreenClaimed(
                existingScreens.size(),
                false
            );

            // Creating a reference changes the cell's reference collection, so
            // defer all reuse/spawn decisions until enumeration has completed.
            for (const auto& targetHandle : targets)
            {
                auto reference = targetHandle.get();
                if (reference == nullptr)
                    continue;

                const auto referenceFormId = reference->GetFormID();
                const auto* baseObject = reference->GetObjectReference();

                REX::INFO(
                    "f4ffmpeg Nuka-Cola target matched by {}: reference {:08X}, base form {:08X}.",
                    source,
                    referenceFormId,
                    baseObject != nullptr
                        ? baseObject->GetFormID()
                        : 0
                );

                {
                    std::scoped_lock lock(
                        injectedNukaColaMachineReferencesMutex
                    );
                    if (injectedNukaColaMachineReferences.contains(
                            referenceFormId))
                    {
                        continue;
                    }

                    injectedNukaColaMachineReferences.emplace(
                        referenceFormId
                    );
                }

                std::optional<std::size_t> matchingScreenIndex;
                for (std::size_t index = 0;
                     index < existingScreens.size();
                     ++index)
                {
                    if (existingScreenClaimed[index])
                        continue;

                    auto screen = existingScreens[index].get();
                    if (screen == nullptr ||
                        !nukaColaScreenMatchesTargetTransform(
                            *screen,
                            *reference))
                    {
                        continue;
                    }

                    matchingScreenIndex = index;
                    break;
                }

                if (matchingScreenIndex)
                {
                    const auto index = *matchingScreenIndex;
                    existingScreenClaimed[index] = true;
                    auto screen = existingScreens[index].get();
                    ++reusedCount;

                    REX::INFO(
                        "f4ffmpeg reused existing Nuka-Cola commercial screen reference {:08X} for target {:08X}; no duplicate was spawned.",
                        screen != nullptr ? screen->GetFormID() : 0,
                        referenceFormId
                    );
                    continue;
                }

                if (injectNukaColaMachineScreen(*reference))
                {
                    ++spawnedCount;
                }
                else
                {
                    std::scoped_lock lock(
                        injectedNukaColaMachineReferencesMutex
                    );
                    injectedNukaColaMachineReferences.erase(
                        referenceFormId
                    );
                }
            }

            REX::INFO(
                "f4ffmpeg scanned Nuka-Cola screen targets from {}: {} reference(s), {} target match(es), {} existing commercial screen(s), {} reused, {} spawned.",
                source,
                referenceCount,
                targetCount,
                existingScreenCount,
                reusedCount,
                spawnedCount
            );
        }

        class nukaColaMachineCellSink final :
            public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESCellFullyLoadedEvent& event,
                RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override
            {
                if (!nukaColaMachineScreenInjectionEnabled || event.cell == nullptr)
                    return RE::BSEventNotifyControl::kContinue;

                if (!nukaColaMachineCellEventObserved.exchange(
                        true,
                        std::memory_order_acq_rel))
                {
                    REX::INFO(
                        "f4ffmpeg received a TESCellFullyLoadedEvent while Nuka-Cola screen injection is enabled."
                    );
                }

                injectNukaColaMachineScreensInCell(
                    *event.cell,
                    "TESCellFullyLoadedEvent"
                );

                return RE::BSEventNotifyControl::kContinue;
            }
        };

        nukaColaMachineCellSink nukaColaMachineCellEventSink;

        using referenceLoad3D_t = RE::NiAVObject* (*) (
            RE::TESObjectREFR*,
            bool
        );

        REL::Relocation<referenceLoad3D_t> originalReferenceLoad3D;

        struct nukaColaSanitizationStats
        {
            std::size_t objectsVisited = 0;
            std::size_t objectControllerChainsCleared = 0;
            std::size_t propertyControllerChainsCleared = 0;
            std::size_t behaviorGraphsRemoved = 0;
            std::size_t videoUvMaterialsExpanded = 0;
            std::size_t videoUvMaterialExpansionFailures = 0;
        };

        bool isIndexedVideoTextureName(
            const char* textureName)
        {
            if (textureName == nullptr || *textureName == '\0')
                return false;

            const std::string key = lowercasePath(
                canonicalTextureLookupPath(textureName)
            );

            return !key.empty() && replacementIndex.contains(key);
        }

        bool isNukaColaCommercialScreenTexture(
            const char* textureName)
        {
            if (textureName == nullptr || *textureName == '\0')
                return false;

            return lowercasePath(
                       canonicalTextureLookupPath(textureName)
                   ) == nukaColaCommercialScreenTexture;
        }

        enum class shaderUvCleanResult : std::uint8_t
        {
            unchanged,
            reset,
            failed
        };

        constexpr float nukaColaCommercialAtlasDimension = 16.0F;

        shaderUvCleanResult expandNukaColaCommercialAtlasUv(
            RE::BSShaderProperty* shaderProperty)
        {
            if (shaderProperty == nullptr || shaderProperty->material == nullptr)
                return shaderUvCleanResult::failed;

            auto* sourceMaterial = shaderProperty->material;

            // NukaColaMachineCommercialFx.nif does NOT store a 1/16 material
            // scale. Its BSEffectShaderProperty is authored at offset (0,0),
            // scale (1,1), while the BSTriShape's packed vertex UVs occupy one
            // 1/16 x 1/16 atlas cell. The two float controllers animate U/V
            // offsets in exact 0.0625 steps over that 16x16 atlas.
            //
            // Once the atlas DDS is replaced with an ordinary video frame, the
            // material therefore has to magnify the mesh UVs by 16 after the
            // offset controllers are detached. Resetting to identity merely
            // leaves the video cropped to its upper-left ~1/16 x 1/16 region.
            bool needsExpansion = false;
            for (std::size_t uvSet = 0; uvSet < 2; ++uvSet)
            {
                const auto& offset = sourceMaterial->texCoordOffset[uvSet];
                const auto& scale = sourceMaterial->texCoordScale[uvSet];

                needsExpansion = needsExpansion ||
                    offset != RE::NiPoint2::ZERO ||
                    scale.x != nukaColaCommercialAtlasDimension ||
                    scale.y != nukaColaCommercialAtlasDimension;
            }

            if (!needsExpansion)
                return shaderUvCleanResult::unchanged;

            auto* cleanMaterial = sourceMaterial->Create();
            if (cleanMaterial == nullptr)
                return shaderUvCleanResult::failed;

            cleanMaterial->CopyMembers(sourceMaterial);

            for (std::size_t uvSet = 0; uvSet < 2; ++uvSet)
            {
                cleanMaterial->texCoordOffset[uvSet] = RE::NiPoint2::ZERO;
                cleanMaterial->texCoordScale[uvSet].x =
                    nukaColaCommercialAtlasDimension;
                cleanMaterial->texCoordScale[uvSet].y =
                    nukaColaCommercialAtlasDimension;
            }

            // Do not mutate sourceMaterial in place: shader materials may be
            // shared across scene instances. SetMaterial(false) lets Fallout
            // hash/deduplicate the atlas-expanded copy and own the material.
            shaderProperty->SetMaterial(cleanMaterial, false);
            delete cleanMaterial;

            // Any passes constructed against the atlas-cell transform are stale.
            shaderProperty->DoClearRenderPasses();
            return shaderUvCleanResult::reset;
        }

        bool nukaColaCommercialSurfaceNeedsSanitization(
            RE::BSShaderProperty* shaderProperty,
            RE::BSGeometry* geometry)
        {
            if (shaderProperty == nullptr)
                return false;

            if (shaderProperty->controllers)
                return true;

            if (shaderProperty->material == nullptr)
                return true;

            for (std::size_t uvSet = 0; uvSet < 2; ++uvSet)
            {
                const auto& offset =
                    shaderProperty->material->texCoordOffset[uvSet];
                const auto& scale =
                    shaderProperty->material->texCoordScale[uvSet];

                if (offset != RE::NiPoint2::ZERO ||
                    scale.x != nukaColaCommercialAtlasDimension ||
                    scale.y != nukaColaCommercialAtlasDimension)
                {
                    return true;
                }
            }

            static const RE::BSFixedString behaviorGraphKey{"BGED"};
            for (RE::NiAVObject* current = geometry;
                 current != nullptr;
                 current = current->parent)
            {
                if (current->controllers ||
                    current->HasExtraData(behaviorGraphKey))
                {
                    return true;
                }
            }

            return false;
        }

        void sanitizeNukaColaMachineScreenObject(
            RE::NiAVObject* object,
            nukaColaSanitizationStats& stats,
            bool videoReplacementActive)
        {
            if (object == nullptr)
                return;

            ++stats.objectsVisited;

            // NiAVObject inherits NiObjectNET. Controllers can be attached to any
            // scene object, not just the NIF root, so clear the complete object
            // controller chain recursively.
            if (object->controllers)
            {
                object->controllers.reset();
                ++stats.objectControllerChainsCleared;
            }

            // The source NIF also carries BSBehaviorGraphExtraData named "BGED"
            // pointing at GenericBehaviors\Autoplay\Autoplay.hkx. Its event
            // sequence includes SoundPlay.DLC04NPCNukaBottleCommercial. When the
            // surface is actively replaced by video, remove that behavior graph
            // as part of the same sanitation pass so the vanilla autoplay/sound
            // machinery cannot continue independently of the swapped texture.
            static const RE::BSFixedString behaviorGraphKey{"BGED"};
            if (object->HasExtraData(behaviorGraphKey) &&
                object->RemoveExtraData(behaviorGraphKey))
            {
                ++stats.behaviorGraphsRemoved;
            }

            // The Nuka-World commercial flipbook has animation state on its shader
            // property. Clear property-local controllers as well, then expand
            // the material UV transform for the exact Nuka texture when video is
            // active. Removing a float controller alone can freeze the last atlas
            // crop; 16x scale maps the baked 1/16-cell mesh UVs back over the full
            // replacement frame.
            if (auto* geometry = netimmerse_cast<RE::BSGeometry*>(object))
            {
                for (auto& property : geometry->properties)
                {
                    if (!property)
                        continue;

                    if (property->controllers)
                    {
                        property->controllers.reset();
                        ++stats.propertyControllerChainsCleared;
                    }

                    auto* shaderProperty =
                        netimmerse_cast<RE::BSShaderProperty*>(property.get());

                    if (shaderProperty == nullptr)
                        continue;

                    const auto* baseTexture = shaderProperty->GetBaseTexture();
                    const char* textureName = baseTexture != nullptr
                        ? baseTexture->name.c_str()
                        : nullptr;

                    if (!videoReplacementActive ||
                        !isNukaColaCommercialScreenTexture(textureName))
                    {
                        continue;
                    }

                    switch (expandNukaColaCommercialAtlasUv(shaderProperty))
                    {
                        case shaderUvCleanResult::reset:
                            ++stats.videoUvMaterialsExpanded;
                            break;
                        case shaderUvCleanResult::failed:
                            ++stats.videoUvMaterialExpansionFailures;
                            break;
                        case shaderUvCleanResult::unchanged:
                        default:
                            break;
                    }
                }
            }

            if (auto* node = netimmerse_cast<RE::NiNode*>(object))
            {
                for (auto& child : node->children)
                {
                    sanitizeNukaColaMachineScreenObject(
                        child.get(),
                        stats,
                        videoReplacementActive
                    );
                }
            }
        }

        RE::NiAVObject* referenceLoad3DHook(
            RE::TESObjectREFR* reference,
            bool backgroundLoading)
        {
            auto* root = originalReferenceLoad3D(reference, backgroundLoading);
            if (reference == nullptr || root == nullptr)
                return root;

            // Sanitization is a correctness consequence of replacing this exact
            // texture, not a separate user policy. This predicate covers both
            // Bethesda-native DLC04 screen references and references spawned by
            // f4ffmpeg because both use the configured commercial-screen base
            // form. Location-only playlists therefore leave vanilla Nuka-World
            // surfaces untouched unless the replacement resolves in that scope.
            const bool activeNukaVideoReplacement =
                isNukaColaCommercialVideoReplacementActive();
            if (!activeNukaVideoReplacement ||
                !isNukaColaCommercialScreenReference(*reference))
            {
                return root;
            }

            nukaColaSanitizationStats stats{};
            sanitizeNukaColaMachineScreenObject(
                root,
                stats,
                true
            );

            REX::INFO(
                "f4ffmpeg sanitized actively replaced Nuka-Cola commercial screen reference {:08X} after Load3D: "
                "{} scene object(s), {} object controller chain(s), "
                "{} property controller chain(s), {} behavior graph(s) removed, "
                "{} video UV material(s) atlas-expanded, {} UV expansion failure(s).",
                reference->GetFormID(),
                stats.objectsVisited,
                stats.objectControllerChainsCleared,
                stats.propertyControllerChainsCleared,
                stats.behaviorGraphsRemoved,
                stats.videoUvMaterialsExpanded,
                stats.videoUvMaterialExpansionFailures
            );
            return root;
        }

        bool installNukaColaActiveReplacementSanitizationHook()
        {
            const bool indexedNukaVideoReplacement =
                isIndexedVideoTextureName(
                    nukaColaCommercialScreenTexture.data()
                );

            // No Nuka replacement can ever resolve if the exact texture is not
            // indexed, so there is nothing for the Load3D sanitizer to do. The
            // hook is independent of optional screen injection.
            if (!indexedNukaVideoReplacement)
                return true;

            constexpr std::size_t referenceLoad3DVtableIndex = 0x86;
            REL::Relocation<std::uintptr_t> vtable{
                RE::TESObjectREFR::VTABLE[0]
            };

            const auto original = *reinterpret_cast<const std::uintptr_t*>(
                vtable.address() + sizeof(void*) * referenceLoad3DVtableIndex
            );
            if (original == 0)
            {
                REX::WARN(
                    "f4ffmpeg could not resolve TESObjectREFR::Load3D; Nuka-Cola screen references will retain their controllers."
                );
                return false;
            }

            originalReferenceLoad3D = original;
            vtable.write_vfunc(
                referenceLoad3DVtableIndex,
                referenceLoad3DHook
            );

            const auto patched = *reinterpret_cast<const std::uintptr_t*>(
                vtable.address() + sizeof(void*) * referenceLoad3DVtableIndex
            );
            if (patched != reinterpret_cast<std::uintptr_t>(&referenceLoad3DHook))
            {
                REX::WARN(
                    "f4ffmpeg failed to patch TESObjectREFR::Load3D; Nuka-Cola screen references will retain their controllers."
                );
                return false;
            }

            REX::INFO(
                "f4ffmpeg Nuka-Cola active-replacement sanitization hook initialized."
            );
            return true;
        }

        bool buildReplacementIndex()
        {
            replacementIndex.clear();

            const auto gameRoot =
                getGameRootPath();

            if (!gameRoot)
                return false;

            const std::filesystem::path root =
                *gameRoot / "Data" / "Video";

            REX::INFO(
                "Scanning loose f4ffmpeg media at '{}' "
                "(supported video containers plus standalone playlist INIs).",
                root.string()
            );

            std::error_code error;

            if (!std::filesystem::is_directory(
                    root,
                    error))
            {
                if (error)
                {
                    REX::WARN(
                        "Unable to inspect loose f4ffmpeg media directory '{}': {}.",
                        root.string(),
                        error.message()
                    );
                }
                else
                {
                    REX::INFO(
                        "No loose f4ffmpeg media directory found at '{}'; "
                        "replacement index is empty.",
                        root.string()
                    );
                }

                return !error;
            }

            std::vector<std::filesystem::path>
                videoFiles;

            std::vector<std::filesystem::path>
                iniFiles;

            std::filesystem::recursive_directory_iterator iterator(
                root,
                std::filesystem::directory_options::skip_permission_denied,
                error
            );

            const std::filesystem::recursive_directory_iterator end;

            if (error)
            {
                REX::WARN(
                    "Unable to enumerate loose f4ffmpeg media directory '{}': {}.",
                    root.string(),
                    error.message()
                );

                return false;
            }

            while (iterator != end)
            {
                std::error_code statusError;

                if (
                    iterator->is_regular_file(
                        statusError
                    ) &&
                    !statusError)
                {
                    const auto& path =
                        iterator->path();

                    if (isSupportedVideoPath(path))
                    {
                        videoFiles.emplace_back(path);
                    }
                    else if (
                        lowercasePath(
                            path.extension().string()
                        ) == ".ini")
                    {
                        iniFiles.emplace_back(path);
                    }
                }

                iterator.increment(error);

                if (error)
                {
                    REX::WARN(
                        "Error while enumerating loose f4ffmpeg media: {}.",
                        error.message()
                    );

                    error.clear();
                }
            }

            const auto sortPaths =
                [](auto& paths)
                {
                    std::sort(
                        paths.begin(),
                        paths.end(),
                        [](const auto& left, const auto& right)
                        {
                            return lowercasePath(
                                       left.generic_string()
                                   ) <
                                   lowercasePath(
                                       right.generic_string()
                                   );
                        }
                    );
                };

            sortPaths(videoFiles);
            sortPaths(iniFiles);

            const auto relativeStemFor =
                [&root](
                    const std::filesystem::path& sourcePath
                ) -> std::optional<std::string>
                {
                    auto relativePath =
                        sourcePath.lexically_relative(
                            root
                        );

                    if (
                        relativePath.empty() ||
                        startsWithInsensitive(
                            relativePath.string(),
                            ".."
                        ))
                    {
                        REX::WARN(
                            "Unable to derive f4ffmpeg replacement key for '{}'.",
                            sourcePath.string()
                        );

                        return std::nullopt;
                    }

                    relativePath.replace_extension();

                    std::string relativeStem =
                        relativePath.string();

                    std::replace(
                        relativeStem.begin(),
                        relativeStem.end(),
                        '/',
                        '\\'
                    );

                    return relativeStem;
                };

            const auto registerReplacementPair =
                [](
                    const std::string& relativeStem,
                    const std::string& initialVideoPath,
                    const std::string& playbackKey,
                    const videoPlaybackSettings& playbackSettings,
                    bool standalonePlaylist,
                    bool hasGlobalPlayback)
                {
                    std::string textureStem{
                        texturesPrefix
                    };

                    textureStem +=
                        relativeStem;

                    std::string vanillaTexture =
                        textureStem;

                    vanillaTexture +=
                        ddsSuffix;

                    std::string directTexture =
                        textureStem;

                    directTexture +=
                        directSwapSuffix;

                    registerIndexedReplacement(
                        std::move(vanillaTexture),
                        indexedVideoReplacement{
                            videoTargetMode::vanillaOverride,
                            initialVideoPath,
                            playbackKey,
                            playbackSettings,
                            standalonePlaylist,
                            hasGlobalPlayback
                        }
                    );

                    registerIndexedReplacement(
                        std::move(directTexture),
                        indexedVideoReplacement{
                            videoTargetMode::directTextureSwap,
                            initialVideoPath,
                            playbackKey,
                            playbackSettings,
                            standalonePlaylist,
                            hasGlobalPlayback
                        }
                    );
                };

            std::size_t activeVideos = 0;
            std::size_t activePlaylists = 0;

            // Existing behavior: a discovered video owns the replacement key and
            // an optional same-stem INI augments its playback policy.
            for (const auto& videoPath : videoFiles)
            {
                const auto relativeStem =
                    relativeStemFor(videoPath);

                if (!relativeStem)
                    continue;

                const std::string physicalVideoPath =
                    videoPath.string();

                const videoPlaybackSettings playbackSettings =
                    loadPlaybackSettingsForVideo(
                        videoPath
                    );
                const bool hasLocationPlayback = std::any_of(
                    playbackSettings.locationOverrides.begin(),
                    playbackSettings.locationOverrides.end(),
                    [](const auto& locationSettings)
                    {
                        return !locationSettings.playlist.empty();
                    }
                );
                // A video without a sidecar remains the historical implicit
                // global replacement. A sidecar that supplies only location
                // media explicitly makes its same-stem video location-only.
                const bool hasGlobalPlayback =
                    !hasLocationPlayback ||
                    !playbackSettings.playlist.empty();

                registerReplacementPair(
                    *relativeStem,
                    hasGlobalPlayback
                        ? physicalVideoPath
                        : std::string{},
                    physicalVideoPath,
                    playbackSettings,
                    false,
                    hasGlobalPlayback
                );

                ++activeVideos;
            }

            // A same-stem INI beside a supported video is already consumed as
            // that video's sidecar above. Any other INI with global or location
            // playlist entries is itself a replacement definition. A location-only
            // definition intentionally has no initial decoder source.
            for (const auto& iniPath : iniFiles)
            {
                if (hasSupportedVideoSibling(iniPath))
                    continue;

                const videoPlaybackSettings playbackSettings =
                    loadPlaybackSettingsFromIni(
                        iniPath
                    );

                const bool hasGlobalPlayback =
                    !playbackSettings.playlist.empty();
                const bool hasLocationPlayback = std::any_of(
                    playbackSettings.locationOverrides.begin(),
                    playbackSettings.locationOverrides.end(),
                    [](const auto& locationSettings)
                    {
                        return !locationSettings.playlist.empty();
                    }
                );

                if (!hasGlobalPlayback && !hasLocationPlayback)
                    continue;

                const auto relativeStem =
                    relativeStemFor(iniPath);

                if (!relativeStem)
                    continue;

                // For standalone playlist INIs, the global playlist ALWAYS wins.
                // Sidecar INIs only use their playlist as a default fallback;
                // the VIDEO's playlist (if any) is authoritative.
                // For a standalone playlist INI (no sibling video):
                // playbackSettings.playlist holds the INI's [Playlist] entries.
                // For a sidecar INI (has sibling video), playbackSettings.playlist
                // holds the INI's entries. The VIDEO's own playlist (if any) is
                // stored in replacement.videoPath.
                const std::string initialVideoPath = hasGlobalPlayback
                    ? playbackSettings.playlist.front()
                    : std::string{};

                if (hasGlobalPlayback &&
                    !(isSupportedVideoUrl(initialVideoPath) ||
                      isSupportedVideoPath(
                          std::filesystem::path{
                              initialVideoPath
                          })))
                {
                    REX::WARN(
                        "f4ffmpeg standalone playlist '{}' has unsupported first media item '{}'; ignoring playlist replacement.",
                        iniPath.string(),
                        initialVideoPath
                    );

                    continue;
                }

                registerReplacementPair(
                    *relativeStem,
                    initialVideoPath,
                    iniPath.string(),
                    playbackSettings,
                    true,
                    hasGlobalPlayback
                );

                ++activePlaylists;

                REX::INFO(
                    "f4ffmpeg indexed standalone playlist '{}' for texture stem '{}' with global playback={}, location playback={}.",
                    iniPath.string(),
                    *relativeStem,
                    hasGlobalPlayback,
                    hasLocationPlayback
                );
            }

            {
                std::scoped_lock activationLock(
                    activationRegistryMutex
                );

                activationByIdentity.clear();

                for (const auto& [textureKey, replacement] :
                     replacementIndex)
                {
                    (void)textureKey;

                    if (!replacement.hasGlobalPlayback)
                        continue;

                    const std::string identityKey =
                        lowercasePath(
                            replacement.playbackKey
                        );

                    if (activationByIdentity.contains(identityKey))
                        continue;

                    auto record =
                        std::make_shared<managerActivationRecord>();

                    record->playbackKey =
                        replacement.playbackKey;
                    record->videoPath =
                        replacement.videoPath;
                    record->playbackSettings =
                        replacement.playbackSettings;

                    activationByIdentity.emplace(
                        identityKey,
                        std::move(record)
                    );
                }
            }

            REX::INFO(
                "f4ffmpeg indexed {} video-backed and {} playlist-backed replacement definition(s) into {} texture mapping(s); decoder activation is lazy and deferred until a mapped texture is requested after game load.",
                activeVideos,
                activePlaylists,
                replacementIndex.size()
            );

            return true;
        }

        std::optional<resolvedVideoTarget>
        resolveVideoTarget(
            const char* texturePath)
        {
            if (
                texturePath == nullptr ||
                *texturePath == '\0')
            {
                return std::nullopt;
            }

            std::string normalized =
                canonicalTextureLookupPath(
                    texturePath
                );

            if (normalized.empty())
                return std::nullopt;

            const std::string key =
                lowercasePath(
                    normalized
                );

            const auto replacement =
                replacementIndex.find(
                    key
                );

            if (replacement == replacementIndex.end())
                return std::nullopt;

            auto locationSettings = resolveLocationPlaybackSettings(
                replacement->second.playbackSettings
            );

            if (!replacement->second.hasGlobalPlayback &&
                (locationSettings.locationKey.empty() ||
                 locationSettings.settings.playlist.empty()))
            {
                return std::nullopt;
            }

            // KEY FIX: For standalone playlist INIs, the [Playlist] global playlist
            // ALWAYS takes precedence. Location playlists from [Location.EditorID.Playlist]
            // in a sidecar INI must NOT incorrectly affect unrelated standalone playlist INIs.
            std::string videoPath = replacement->second.videoPath;

            if (replacement->second.standalonePlaylist &&
                !replacement->second.playbackSettings.playlist.empty())
            {
                REX::DEBUG("  -> using global playlist: {} (standalone INI always uses its own playlist)",
                             replacement->second.playbackSettings.playlist.front());
                videoPath =
                    std::string(replacement->second.playbackSettings.playlist.front());
            }
            else if (!replacement->second.playbackSettings.playlist.empty())
            {
                // Sidecar INI with global playlist
                videoPath =
                    std::string(replacement->second.playbackSettings.playlist.front());
            }
            else if (!locationSettings.settings.playlist.empty())
            {
                // No global, use location playlist as fallback
                videoPath =
                    std::string(locationSettings.settings.playlist.front());
            }
            std::string playbackKey = replacement->second.playbackKey;
            if (!locationSettings.locationKey.empty())
            {
                playbackKey += "|location:";
                playbackKey += locationSettings.locationKey;
            }

            return resolvedVideoTarget{
                replacement->second.mode,
                std::move(normalized),
                std::move(videoPath),
                std::move(playbackKey),
                std::move(locationSettings.settings)
            };
        }

        std::mutex targetRegistryMutex;

        std::unordered_map<
            std::string,
            std::shared_ptr<videoTarget>>
            targetsByTexture;

        void requestPlaybackActivation(
            std::string_view playbackKey)
        {
            if (playbackKey.empty())
                return;

            const std::string identityKey =
                lowercasePath(playbackKey);

            std::shared_ptr<managerActivationRecord> record;

            {
                std::scoped_lock activationLock(
                    activationRegistryMutex
                );

                const auto existing =
                    activationByIdentity.find(identityKey);

                if (existing == activationByIdentity.end())
                    return;

                record = existing->second;
            }

            auto expected =
                managerActivationState::dormant;

            if (!record->state.compare_exchange_strong(
                    expected,
                    managerActivationState::queued,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                return;
            }

            {
                std::scoped_lock queueLock(
                    activationQueueMutex
                );

                activationQueue.emplace_back(record);
            }

            REX::DEBUG(
                "f4ffmpeg queued lazy manager activation for playback definition '{}' (initial source '{}').",
                record->playbackKey,
                record->videoPath
            );

            activationQueueCv.notify_one();
        }

        void ensurePlaybackActivationRecord(
            std::string_view playbackKey,
            std::string_view videoPath,
            const videoPlaybackSettings& playbackSettings)
        {
            if (playbackKey.empty() || videoPath.empty())
                return;

            const std::string identityKey = lowercasePath(playbackKey);
            std::scoped_lock activationLock(activationRegistryMutex);

            if (activationByIdentity.contains(identityKey))
                return;

            auto record = std::make_shared<managerActivationRecord>();
            record->playbackKey = playbackKey;
            record->videoPath = videoPath;
            record->playbackSettings = playbackSettings;
            activationByIdentity.emplace(identityKey, std::move(record));
        }

        void activationWorkerMain()
        {
            REX::INFO(
                "f4ffmpeg lazy manager activation worker started."
            );

            while (true)
            {
                std::shared_ptr<managerActivationRecord> record;

                {
                    std::unique_lock queueLock(
                        activationQueueMutex
                    );

                    activationQueueCv.wait(
                        queueLock,
                        []()
                        {
                            return activationStopRequested ||
                                (activationEnabled &&
                                 !activationQueue.empty());
                        }
                    );

                    if (activationStopRequested)
                        break;

                    record = activationQueue.front();
                    activationQueue.pop_front();
                }

                if (!record)
                    continue;

                auto expected =
                    managerActivationState::queued;

                if (!record->state.compare_exchange_strong(
                        expected,
                        managerActivationState::activating,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    continue;
                }

                const std::string identityKey =
                    lowercasePath(record->playbackKey);

                std::shared_ptr<manager> playback;

                {
                    std::scoped_lock playbackLock(
                        playbackRegistryMutex
                    );

                    if (const auto existing =
                            playbackByIdentity.find(identityKey);
                        existing != playbackByIdentity.end())
                    {
                        playback = existing->second;
                    }
                }

                if (!playback)
                {
                    playback =
                        createManager(
                            record->videoPath.c_str(),
                            record->playbackSettings
                        );
                }

                if (!playback)
                {
                    record->state.store(
                        managerActivationState::failed,
                        std::memory_order_release
                    );

                    REX::WARN(
                        "f4ffmpeg lazy activation could not start manager for playback definition '{}' (initial source '{}'); vanilla texture presentation will remain active.",
                        record->playbackKey,
                        record->videoPath
                    );

                    continue;
                }

                {
                    std::scoped_lock playbackLock(
                        playbackRegistryMutex
                    );

                    playbackByIdentity[identityKey] =
                        playback;
                }

                {
                    std::scoped_lock targetLock(
                        targetRegistryMutex
                    );

                    for (auto& [textureKey, target] :
                         targetsByTexture)
                    {
                        (void)textureKey;

                        if (
                            !target ||
                            lowercasePath(target->getPlaybackKey()) !=
                                identityKey)
                        {
                            continue;
                        }

                        target->attachPlayback(
                            playback
                        );
                    }
                }

                record->state.store(
                    managerActivationState::active,
                    std::memory_order_release
                );

                REX::INFO(
                    "f4ffmpeg lazily activated manager for playback definition '{}' (initial source '{}').",
                    record->playbackKey,
                    record->videoPath
                );
            }

        }

        struct activationWorkerLifetime
        {
            ~activationWorkerLifetime()
            {
                {
                    std::scoped_lock queueLock(
                        activationQueueMutex
                    );

                    activationStopRequested = true;
                }

                activationQueueCv.notify_all();

                if (activationWorker.joinable())
                {
                    activationWorker.join();
                }
            }
        };

        activationWorkerLifetime activationLifetime;

        using textureType =
            RE::BSShaderProperty::TextureTypeEnum;

        using getTexturePrefetched_t = void(*)(
            RE::BSShaderTextureSet*,
            const void*,
            textureType,
            RE::NiPointer<RE::NiTexture>*,
            bool
        );

        using getTexture_t = void(*)(
            RE::BSShaderTextureSet*,
            textureType,
            RE::NiPointer<RE::NiTexture>*,
            bool
        );

        using getTextureFilename_t = const char*(*)(
            RE::BSShaderTextureSet*,
            textureType
        );

        REL::Relocation<getTextureFilename_t>
            originalGetTextureFilename;

        REL::Relocation<getTexturePrefetched_t>
            originalGetTexturePrefetched;

        REL::Relocation<getTexture_t>
            originalGetTexture;

        std::atomic_bool filenameHookObserved = false;
        std::atomic_bool prefetchedHookObserved = false;
        std::atomic_bool ordinaryHookObserved = false;

        std::mutex diagnosticTextureMutex;
        std::unordered_set<std::string>
            diagnosticTexturePaths;

        void diagnoseResolvedTexturePath(
            const char* hookName,
            const char* texturePath)
        {
            if (
                texturePath == nullptr ||
                *texturePath == '\0')
            {
                return;
            }

            const std::string normalized =
                canonicalTextureLookupPath(
                    texturePath
                );

            const std::string key =
                lowercasePath(
                    normalized
                );

            const bool indexed =
                replacementIndex.find(key) !=
                    replacementIndex.end();

            const bool tvRelated =
                key.find("tvanim") != std::string::npos ||
                key.find("standby") != std::string::npos ||
                key.find("television") != std::string::npos;

            if (!indexed && !tvRelated)
                return;

            std::string diagnosticKey{
                hookName
                    ? hookName
                    : "unknown"
            };

            diagnosticKey += "|";
            diagnosticKey += key;

            {
                std::scoped_lock lock(
                    diagnosticTextureMutex
                );

                if (!diagnosticTexturePaths.emplace(
                        std::move(diagnosticKey)
                    ).second)
                {
                    return;
                }
            }

            if (indexed)
            {
                REX::INFO(
                    "f4ffmpeg {} observed indexed texture '{}'.",
                    hookName,
                    normalized
                );
            }
            else
            {
                REX::INFO(
                    "f4ffmpeg {} observed TV-related texture '{}' "
                    "with no indexed replacement.",
                    hookName,
                    normalized
                );
            }
        }

        void diagnoseTexturePath(
            const char* hookName,
            textureType type,
            const char* texturePath)
        {
            if (type != textureType::kBase)
                return;

            diagnoseResolvedTexturePath(
                hookName,
                texturePath
            );
        }

        const char* getTextureFilenameHook(
            RE::BSShaderTextureSet* textureSet,
            textureType type)
        {
            const char* texturePath =
                originalGetTextureFilename(
                    textureSet,
                    type
                );

            if (!filenameHookObserved.exchange(
                    true,
                    std::memory_order_acq_rel
                ))
            {
                REX::INFO(
                    "f4ffmpeg BSShaderTextureSet::GetTextureFilename "
                    "hook reached (first type={}).",
                    static_cast<std::uint32_t>(type)
                );
            }

            diagnoseTexturePath(
                "GetTextureFilename",
                type,
                texturePath
            );

            return texturePath;
        }

        // F4SE's public reverse-engineered BSRenderData layout places the
        // ID3D11ShaderResourceView at offset 0. CommonLib exposes the containing
        // object as opaque BSGraphics::Texture, so only this first pointer is
        // observed. We never mutate the object.
        struct rendererTexturePrefix
        {
            REX::W32::ID3D11ShaderResourceView*
                resourceView = nullptr;
        };

        static_assert(
            offsetof(
                rendererTexturePrefix,
                resourceView
            ) == 0
        );

        std::shared_mutex bindingMutex;

        std::unordered_map<
            REX::W32::ID3D11ShaderResourceView*,
            std::shared_ptr<const videoTarget>>
            targetsByVanillaSrv;

        // Preserve the source texture identity even when its current player
        // location has no active video target.
        std::unordered_map<
            REX::W32::ID3D11ShaderResourceView*,
            std::string>
            texturePathsByVanillaSrv;

        std::unordered_set<
            REX::W32::ID3D11ShaderResourceView*>
            suppressedVanillaSrvs;

        bool workshopTvRasterScanSuppressionEnabled =
            false;

        bool workshopTvStaticSuppressionEnabled =
            false;

        bool workshopTvWarpSuppressionEnabled =
            false;

        std::atomic_bool rasterScanSuppressionObserved =
            false;

        std::atomic_bool hasPresentationBindings =
            false;

        std::atomic_bool presentationObserved =
            false;

        std::atomic_bool psHookObserved =
            false;

        std::mutex observedBindingMutex;

        std::unordered_set<std::string>
            observedBindings;

        std::mutex bindingFailureMutex;
        std::unordered_set<std::string>
            bindingFailures;

        void logBindingFailureOnce(
            const char* texturePath,
            const char* reason)
        {
            std::string key =
                lowercasePath(
                    canonicalTextureLookupPath(
                        texturePath
                            ? texturePath
                            : ""
                    )
                );

            key += "|";
            key += reason
                ? reason
                : "unknown";

            {
                std::scoped_lock lock(
                    bindingFailureMutex
                );

                if (!bindingFailures.emplace(
                        std::move(key)
                    ).second)
                {
                    return;
                }
            }

            REX::INFO(
                "f4ffmpeg handled texture '{}' reached acquisition, "
                "but presentation binding is not ready: {}.",
                texturePath
                    ? texturePath
                    : "<null>",
                reason
                    ? reason
                    : "unknown"
            );
        }

        REX::W32::ID3D11ShaderResourceView*
        getNiTextureSrv(
            const char* texturePath,
            RE::NiTexture* niTexture)
        {
            if (niTexture == nullptr)
            {
                logBindingFailureOnce(
                    texturePath,
                    "NiTexture pointer is null"
                );

                return nullptr;
            }

            if (niTexture->rendererTexture == nullptr)
            {
                logBindingFailureOnce(
                    texturePath,
                    "NiTexture has no rendererTexture yet"
                );

                return nullptr;
            }

            const auto* rendererTexture =
                reinterpret_cast<
                    const rendererTexturePrefix*>(
                        niTexture->rendererTexture
                    );

            auto* srv =
                rendererTexture->resourceView;

            if (srv == nullptr)
            {
                logBindingFailureOnce(
                    texturePath,
                    "rendererTexture has no shader resource view"
                );
            }

            return srv;
        }

        bool registerNiTexturePresentationBinding(
            const char* texturePath,
            RE::NiTexture* niTexture,
            const char* sourceName)
        {
            auto target =
                getVideoTargetForTexture(
                    texturePath
                );

            const std::string indexedTexturePath =
                canonicalTextureLookupPath(
                    texturePath
                        ? texturePath
                        : ""
                );
            const bool indexed = replacementIndex.contains(
                lowercasePath(indexedTexturePath)
            );

            if (!target && !indexed)
                return true;

            auto* vanillaSrv =
                getNiTextureSrv(
                    texturePath,
                    niTexture
                );

            if (vanillaSrv == nullptr)
                return false;

            bool changed = false;

            {
                std::unique_lock lock(
                    bindingMutex
                );

                changed = texturePathsByVanillaSrv.emplace(
                    vanillaSrv,
                    indexedTexturePath
                ).second;

                const auto existing =
                    targetsByVanillaSrv.find(
                        vanillaSrv
                    );

                if (target && existing == targetsByVanillaSrv.end())
                {
                    targetsByVanillaSrv.emplace(
                        vanillaSrv,
                        target
                    );

                    changed = true;
                }
                else if (
                    target &&
                    existing->second->getVideoPath() !=
                        target->getVideoPath() &&
                    modePriority(target->getMode()) >
                        modePriority(existing->second->getMode()))
                {
                    existing->second =
                        target;

                    changed = true;
                }
            }

            hasPresentationBindings.store(
                true,
                std::memory_order_release
            );

            if (!changed)
                return true;

            if (!target)
            {
                REX::INFO(
                    "Bound location-only f4ffmpeg presentation '{}' via {}.",
                    indexedTexturePath,
                    sourceName
                        ? sourceName
                        : "unknown"
                );
                return true;
            }

            std::string logKey =
                lowercasePath(
                    canonicalTextureLookupPath(
                        texturePath
                            ? texturePath
                            : ""
                    )
                );

            logKey += "|";
            logKey += sourceName
                ? sourceName
                : "unknown";

            {
                std::scoped_lock lock(
                    observedBindingMutex
                );

                if (!observedBindings.emplace(
                        std::move(logKey)
                    ).second)
                {
                    return true;
                }
            }

            REX::INFO(
                "Bound f4ffmpeg {} presentation '{}' -> '{}' via {}.",
                modeName(target->getMode()),
                target->getTexturePath(),
                target->getVideoPath(),
                sourceName
                    ? sourceName
                    : "unknown"
            );

            return true;
        }

        bool registerNiTextureSuppressionBinding(
            const char* texturePath,
            RE::NiTexture* niTexture,
            const char* sourceName)
        {
            auto* vanillaSrv =
                getNiTextureSrv(
                    texturePath,
                    niTexture
                );

            if (vanillaSrv == nullptr)
                return false;

            bool changed = false;

            {
                std::unique_lock lock(
                    bindingMutex
                );

                changed =
                    suppressedVanillaSrvs.emplace(
                        vanillaSrv
                    ).second;
            }

            if (!changed)
                return true;

            hasPresentationBindings.store(
                true,
                std::memory_order_release
            );

            REX::INFO(
                "Bound target-local f4ffmpeg TV raster-scan suppression '{}' via {}.",
                canonicalTextureLookupPath(
                    texturePath
                        ? texturePath
                        : ""
                ),
                sourceName
                    ? sourceName
                    : "unknown"
            );

            return true;
        }

        void registerPresentationBinding(
            RE::BSShaderTextureSet* textureSet,
            textureType type,
            RE::NiPointer<RE::NiTexture>* texture)
        {
            if (
                textureSet == nullptr ||
                type != textureType::kBase)
            {
                return;
            }

            const char* texturePath =
                originalGetTextureFilename(
                    textureSet,
                    type
                );

            if (
                texture == nullptr ||
                !*texture)
            {
                if (getVideoTargetForTexture(texturePath))
                {
                    logBindingFailureOnce(
                        texturePath,
                        "Bethesda returned no NiTexture yet"
                    );
                }

                return;
            }

            registerNiTexturePresentationBinding(
                texturePath,
                texture->get(),
                "BSShaderTextureSet"
            );
        }

        void getTexturePrefetchedHook(
            RE::BSShaderTextureSet* textureSet,
            const void* prefetchedHandle,
            textureType type,
            RE::NiPointer<RE::NiTexture>* texture,
            bool srgb)
        {
            // Bethesda always owns acquisition. f4ffmpeg only registers a
            // presentation override for the object Bethesda actually returned.
            originalGetTexturePrefetched(
                textureSet,
                prefetchedHandle,
                type,
                texture,
                srgb
            );

            if (!prefetchedHookObserved.exchange(
                    true,
                    std::memory_order_acq_rel
                ))
            {
                REX::INFO(
                    "f4ffmpeg BSShaderTextureSet::GetTexture(prefetched) "
                    "hook reached (first type={}).",
                    static_cast<std::uint32_t>(type)
                );
            }

            diagnoseTexturePath(
                "GetTexture(prefetched)",
                type,
                originalGetTextureFilename(
                    textureSet,
                    type
                )
            );

            registerPresentationBinding(
                textureSet,
                type,
                texture
            );
        }

        void getTextureHook(
            RE::BSShaderTextureSet* textureSet,
            textureType type,
            RE::NiPointer<RE::NiTexture>* texture,
            bool srgb)
        {
            originalGetTexture(
                textureSet,
                type,
                texture,
                srgb
            );

            if (!ordinaryHookObserved.exchange(
                    true,
                    std::memory_order_acq_rel
                ))
            {
                REX::INFO(
                    "f4ffmpeg BSShaderTextureSet::GetTexture ordinary "
                    "hook reached (first type={}).",
                    static_cast<std::uint32_t>(type)
                );
            }

            diagnoseTexturePath(
                "GetTexture",
                type,
                originalGetTextureFilename(
                    textureSet,
                    type
                )
            );

            registerPresentationBinding(
                textureSet,
                type,
                texture
            );
        }

        struct msvcCompleteObjectLocator64
        {
            std::uint32_t signature = 0;
            std::uint32_t offset = 0;
            std::uint32_t constructorDisplacementOffset = 0;
            std::int32_t typeDescriptorRva = 0;
            std::int32_t classDescriptorRva = 0;
            std::int32_t selfRva = 0;
        };

        static_assert(
            sizeof(msvcCompleteObjectLocator64) == 0x18
        );

        template <class T>
        T readUnaligned(
            std::uintptr_t address)
        {
            T value{};

            std::memcpy(
                &value,
                reinterpret_cast<const void*>(
                    address
                ),
                sizeof(T)
            );

            return value;
        }

        bool containsAddressRange(
            const REX::FModuleSection& segment,
            std::uintptr_t address,
            std::size_t size)
        {
            if (size > segment.GetSize())
                return false;

            const auto begin =
                segment.GetAddress();

            const auto end =
                begin + segment.GetSize();

            return
                address >= begin &&
                address <= end - size;
        }

        std::optional<std::uintptr_t>
        findBytesInSegment(
            const REX::FModuleSection& segment,
            std::string_view bytes)
        {
            if (
                bytes.empty() ||
                bytes.size() + 1 > segment.GetSize())
            {
                return std::nullopt;
            }

            const auto* begin =
                reinterpret_cast<const char*>(
                    segment.GetAddress()
                );

            const auto* end =
                begin + segment.GetSize() -
                    bytes.size() - 1;

            for (const char* current = begin;
                 current <= end;
                 ++current)
            {
                if (
                    std::memcmp(
                        current,
                        bytes.data(),
                        bytes.size()
                    ) == 0 &&
                    current[bytes.size()] == '\0')
                {
                    return reinterpret_cast<
                        std::uintptr_t>(
                            current
                        );
                }
            }

            return std::nullopt;
        }

        std::optional<std::uintptr_t>
        findEffectShaderPropertyTypeDescriptor()
        {
            const auto module =
                REX::FModule::GetExecutingModule();

            const auto data =
                module.GetSection(".data");

            const auto rdata =
                module.GetSection(".rdata");

            // Fallout's engine types live in the executable's global namespace.
            // Keep the RE-qualified spelling as a harmless fallback for forks or
            // alternate symbol-generation conventions.
            static constexpr std::array names{
                std::string_view{
                    ".?AVBSEffectShaderProperty@@"
                },
                std::string_view{
                    ".?AVBSEffectShaderProperty@RE@@"
                }
            };

            for (const auto name : names)
            {
                for (const auto* segment :
                     std::array{
                         &data,
                         &rdata
                     })
                {
                    const auto nameAddress =
                        findBytesInSegment(
                            *segment,
                            name
                        );

                    if (!nameAddress)
                        continue;

                    constexpr std::size_t
                        typeDescriptorPrefixSize =
                            sizeof(void*) * 2;

                    if (
                        *nameAddress <
                        segment->GetAddress() +
                            typeDescriptorPrefixSize)
                    {
                        continue;
                    }

                    const auto typeDescriptor =
                        *nameAddress -
                        typeDescriptorPrefixSize;

                    REX::INFO(
                        "f4ffmpeg located BSEffectShaderProperty MSVC RTTI "
                        "type descriptor at {} (name '{}').",
                        reinterpret_cast<void*>(
                            typeDescriptor
                        ),
                        name
                    );

                    return typeDescriptor;
                }
            }

            return std::nullopt;
        }

        std::optional<std::uintptr_t>
        findEffectShaderPropertyVtable()
        {
            const auto module =
                REX::FModule::GetExecutingModule();

            const auto moduleBase =
                module.GetBaseAddress();

            const auto rdata =
                module.GetSection(".rdata");

            const auto typeDescriptor =
                findEffectShaderPropertyTypeDescriptor();

            if (!typeDescriptor)
                return std::nullopt;

            const auto typeDescriptorOffset =
                *typeDescriptor -
                moduleBase;

            if (
                typeDescriptorOffset >
                static_cast<std::uintptr_t>(
                    std::numeric_limits<std::int32_t>::max()
                ))
            {
                return std::nullopt;
            }

            const auto typeDescriptorRva =
                static_cast<std::int32_t>(
                    typeDescriptorOffset
                );

            std::vector<std::uintptr_t>
                completeObjectLocators;

            const auto rdataBegin =
                rdata.GetAddress();

            const auto rdataEnd =
                rdataBegin +
                rdata.GetSize();

            for (
                std::uintptr_t address =
                    rdataBegin;
                address +
                    sizeof(msvcCompleteObjectLocator64) <=
                    rdataEnd;
                address += alignof(std::uint32_t))
            {
                const auto locator =
                    readUnaligned<
                        msvcCompleteObjectLocator64>(
                            address
                        );

                if (
                    locator.signature != 1 ||
                    locator.typeDescriptorRva !=
                        typeDescriptorRva)
                {
                    continue;
                }

                const auto selfOffset =
                    address -
                    moduleBase;

                if (
                    selfOffset >
                    static_cast<std::uintptr_t>(
                        std::numeric_limits<std::int32_t>::max()
                    ) ||
                    locator.selfRva !=
                        static_cast<std::int32_t>(
                            selfOffset
                        ))
                {
                    continue;
                }

                completeObjectLocators.emplace_back(
                    address
                );
            }

            if (completeObjectLocators.empty())
                return std::nullopt;

            std::vector<std::uintptr_t>
                vtables;

            for (const auto locatorAddress :
                 completeObjectLocators)
            {
                for (
                    std::uintptr_t address =
                        rdataBegin;
                    address + sizeof(std::uintptr_t) <=
                        rdataEnd;
                    address += alignof(std::uintptr_t))
                {
                    if (
                        readUnaligned<std::uintptr_t>(
                            address
                        ) != locatorAddress)
                    {
                        continue;
                    }

                    const auto vtable =
                        address +
                        sizeof(std::uintptr_t);

                    const auto requiredSize =
                        sizeof(std::uintptr_t) *
                        (effectGetBaseTextureVtableIndex + 1);

                    if (!containsAddressRange(
                            rdata,
                            vtable,
                            requiredSize
                        ))
                    {
                        continue;
                    }

                    const auto firstVirtual =
                        readUnaligned<std::uintptr_t>(
                            vtable
                        );

                    const auto getBaseTexture =
                        readUnaligned<std::uintptr_t>(
                            vtable +
                            sizeof(std::uintptr_t) *
                                effectGetBaseTextureVtableIndex
                        );

                    if (
                        firstVirtual == 0 ||
                        getBaseTexture == 0)
                    {
                        continue;
                    }

                    if (
                        std::find(
                            vtables.begin(),
                            vtables.end(),
                            vtable
                        ) == vtables.end())
                    {
                        vtables.emplace_back(
                            vtable
                        );
                    }
                }
            }

            if (vtables.size() != 1)
            {
                REX::ERROR(
                    "f4ffmpeg BSEffectShaderProperty RTTI produced {} "
                    "candidate vtable(s); refusing to patch an ambiguous target.",
                    vtables.size()
                );

                return std::nullopt;
            }

            return vtables.front();
        }

        using effectGetBaseTexture_t =
            RE::NiTexture* (*)(
                const RE::BSShaderProperty*
            );

        using effectGetRenderPasses_t =
            RE::BSShaderProperty::RenderPassArray* (*)(
                RE::BSShaderProperty*,
                RE::BSGeometry*,
                std::uint32_t,
                RE::BSShaderAccumulator*
            );

        REL::Relocation<effectGetBaseTexture_t>
            originalEffectGetBaseTexture;

        REL::Relocation<effectGetRenderPasses_t>
            originalEffectGetRenderPasses;

        std::atomic_bool effectRenderPassHookObserved =
            false;

        std::atomic_bool effectBaseTextureHookObserved =
            false;

        enum class workshopTvEffectKind : std::uint8_t
        {
            other,
            rasterScan,
            staticFuzz,
            screenWarp
        };

        std::shared_mutex touchedTvNodesMutex;

        // BSShaderProperty::fadeNode is the cheapest ownership key and works for
        // many TV NIFs, but not every asset gives all sibling effect properties
        // the same fadeNode. Keep it as the fast path and supplement it with a
        // shallow scene-graph ancestry association keyed from the handled screen
        // geometry. Limiting ancestry depth keeps the fallback local to the placed
        // NIF instead of eventually matching a broad cell/world scene root.
        std::unordered_set<const void*>
            touchedTvNodes;

        constexpr std::size_t maxTvOwnershipAncestorDepth = 4;

        std::shared_mutex touchedTvGeometryAncestorsMutex;
        std::unordered_set<const RE::NiAVObject*>
            touchedTvGeometryAncestors;

        std::mutex tvGeometryAssociationDiagnosticMutex;
        std::unordered_set<const RE::BSGeometry*>
            tvGeometryAssociationDiagnostics;

        // Once a touched-TV effect property has been classified and its matching
        // config option is enabled, GetBaseTexture returns nullptr for this exact
        // property. Keep this property-local instead of blacklisting the texture's
        // SRV globally; shared utility textures can then continue rendering on
        // untouched TVs and unrelated NIFs.
        std::shared_mutex nulledTvEffectPropertiesMutex;
        std::unordered_set<const RE::BSShaderProperty*>
            nulledTvEffectProperties;

        std::mutex tvEffectDiagnosticMutex;
        std::unordered_set<std::string>
            tvEffectDiagnostics;

        const void* getEffectOwnerKey(
            const RE::BSShaderProperty* shaderProperty)
        {
            if (
                shaderProperty == nullptr ||
                shaderProperty->fadeNode == nullptr)
            {
                return nullptr;
            }

            return static_cast<const void*>(
                shaderProperty->fadeNode
            );
        }

        void markTvNodeTouched(
            const RE::BSShaderProperty* shaderProperty)
        {
            const void* owner =
                getEffectOwnerKey(
                    shaderProperty
                );

            if (owner == nullptr)
                return;

            std::unique_lock lock(
                touchedTvNodesMutex
            );

            touchedTvNodes.emplace(
                owner
            );
        }

        bool isTvNodeTouched(
            const RE::BSShaderProperty* shaderProperty)
        {
            const void* owner =
                getEffectOwnerKey(
                    shaderProperty
                );

            if (owner == nullptr)
                return false;

            std::shared_lock lock(
                touchedTvNodesMutex
            );

            return touchedTvNodes.find(
                       owner
                   ) != touchedTvNodes.end();
        }

        void markTvGeometryTouched(
            const RE::BSGeometry* geometry)
        {
            if (geometry == nullptr)
                return;

            std::array<
                const RE::NiAVObject*,
                maxTvOwnershipAncestorDepth>
                ancestors{};

            std::size_t count = 0;
            const RE::NiAVObject* current = geometry;

            while (
                current != nullptr &&
                current->parent != nullptr &&
                count < ancestors.size())
            {
                current = current->parent;
                ancestors[count++] = current;
            }

            if (count == 0)
                return;

            std::unique_lock lock(
                touchedTvGeometryAncestorsMutex
            );

            for (std::size_t index = 0; index < count; ++index)
            {
                touchedTvGeometryAncestors.emplace(
                    ancestors[index]
                );
            }
        }

        bool isTvGeometryTouched(
            const RE::BSGeometry* geometry)
        {
            if (geometry == nullptr)
                return false;

            const RE::NiAVObject* current = geometry;

            std::shared_lock lock(
                touchedTvGeometryAncestorsMutex
            );

            for (
                std::size_t depth = 0;
                depth < maxTvOwnershipAncestorDepth &&
                    current != nullptr &&
                    current->parent != nullptr;
                ++depth)
            {
                current = current->parent;

                if (touchedTvGeometryAncestors.find(current) !=
                    touchedTvGeometryAncestors.end())
                {
                    return true;
                }
            }

            return false;
        }

        bool isTvInstanceTouched(
            const RE::BSShaderProperty* shaderProperty,
            const RE::BSGeometry* geometry)
        {
            if (isTvNodeTouched(shaderProperty))
                return true;

            if (!isTvGeometryTouched(geometry))
                return false;

            // Once scene ancestry proves this property belongs to a handled TV,
            // promote its fadeNode too. Subsequent sibling effects on the same
            // alternate fadeNode then take the cheap path.
            markTvNodeTouched(shaderProperty);

            bool logAssociation = false;

            {
                std::scoped_lock lock(
                    tvGeometryAssociationDiagnosticMutex
                );

                logAssociation =
                    tvGeometryAssociationDiagnostics.emplace(
                        geometry
                    ).second;
            }

            if (logAssociation)
            {
                REX::INFO(
                    "f4ffmpeg associated touched-TV geometry '{}' via scene-graph ancestry fallback.",
                    geometry != nullptr && geometry->name.c_str() != nullptr
                        ? geometry->name.c_str()
                        : "<unnamed>"
                );
            }

            return true;
        }

        void appendEffectSignaturePart(
            std::string& signature,
            const char* value)
        {
            if (
                value == nullptr ||
                *value == '\0')
            {
                return;
            }

            if (!signature.empty())
                signature += '|';

            signature +=
                lowercasePath(
                    value
                );
        }

        std::string buildEffectSignature(
            const RE::BSShaderProperty* shaderProperty,
            const RE::BSGeometry* geometry,
            const RE::NiTexture* baseTexture)
        {
            std::string signature;

            if (geometry != nullptr)
            {
                appendEffectSignaturePart(
                    signature,
                    geometry->name.c_str()
                );
            }

            if (shaderProperty != nullptr)
            {
                appendEffectSignaturePart(
                    signature,
                    shaderProperty->name.c_str()
                );
            }

            if (baseTexture != nullptr)
            {
                appendEffectSignaturePart(
                    signature,
                    baseTexture->name.c_str()
                );
            }

            return signature;
        }

        bool signatureContainsAny(
            std::string_view signature,
            std::initializer_list<std::string_view> tokens)
        {
            for (const auto token : tokens)
            {
                if (
                    signature.find(token) !=
                        std::string_view::npos)
                {
                    return true;
                }
            }

            return false;
        }

        workshopTvEffectKind classifyWorkshopTvEffect(
            const RE::BSShaderProperty* shaderProperty,
            const RE::BSGeometry* geometry,
            const RE::NiTexture* baseTexture)
        {
            if (
                baseTexture != nullptr &&
                isWorkshopTvRasterScanTexture(
                    baseTexture->name.c_str()
                ))
            {
                return workshopTvEffectKind::rasterScan;
            }

            const std::string signature =
                buildEffectSignature(
                    shaderProperty,
                    geometry,
                    baseTexture
                );

            // Bethesda's TV is authored as separate NIF effect geometries for
            // distortion/warp and animated noise/static. Prefer semantic NIF /
            // material names over generic texture paths so shared utility
            // textures (notably ColorBlackUtility.dds) are never globally killed.
            if (signatureContainsAny(
                    signature,
                    {
                        "warp",
                        "distort",
                        "distortion",
                        "wobble"
                    }))
            {
                return workshopTvEffectKind::screenWarp;
            }

            if (signatureContainsAny(
                    signature,
                    {
                        "static",
                        "fuzz",
                        "noise",
                        "interference"
                    }))
            {
                return workshopTvEffectKind::staticFuzz;
            }

            return workshopTvEffectKind::other;
        }

        const char* workshopTvEffectKindName(
            workshopTvEffectKind kind)
        {
            switch (kind)
            {
                case workshopTvEffectKind::rasterScan:
                    return "raster scan";

                case workshopTvEffectKind::staticFuzz:
                    return "static/fuzz";

                case workshopTvEffectKind::screenWarp:
                    return "screen warp";

                case workshopTvEffectKind::other:
                default:
                    return "unclassified";
            }
        }

        void diagnoseTouchedTvEffect(
            const RE::BSShaderProperty* shaderProperty,
            const RE::BSGeometry* geometry,
            const RE::NiTexture* baseTexture,
            workshopTvEffectKind kind)
        {
            const std::string geometryName =
                geometry != nullptr &&
                geometry->name.c_str() != nullptr
                    ? geometry->name.c_str()
                    : "<unnamed>";

            const std::string propertyName =
                shaderProperty != nullptr &&
                shaderProperty->name.c_str() != nullptr
                    ? shaderProperty->name.c_str()
                    : "<unnamed>";

            const std::string textureName =
                baseTexture != nullptr &&
                baseTexture->name.c_str() != nullptr
                    ? baseTexture->name.c_str()
                    : "<none>";

            std::string key =
                lowercasePath(
                    geometryName + "|" +
                    propertyName + "|" +
                    textureName
                );

            {
                std::scoped_lock lock(
                    tvEffectDiagnosticMutex
                );

                if (!tvEffectDiagnostics.emplace(
                        std::move(key)
                    ).second)
                {
                    return;
                }
            }

            REX::INFO(
                "f4ffmpeg touched-TV effect: kind={}, geometry='{}', shader='{}', baseTexture='{}'.",
                workshopTvEffectKindName(kind),
                geometryName,
                propertyName,
                textureName
            );
        }

        bool shouldSuppressWorkshopTvEffect(
            workshopTvEffectKind kind)
        {
            switch (kind)
            {
                case workshopTvEffectKind::rasterScan:
                    return workshopTvRasterScanSuppressionEnabled;

                case workshopTvEffectKind::staticFuzz:
                    return workshopTvStaticSuppressionEnabled;

                case workshopTvEffectKind::screenWarp:
                    return workshopTvWarpSuppressionEnabled;

                default:
                    return false;
            }
        }

        bool markWorkshopTvEffectTextureNull(
            const RE::BSShaderProperty* shaderProperty,
            workshopTvEffectKind kind)
        {
            if (
                shaderProperty == nullptr ||
                !shouldSuppressWorkshopTvEffect(kind))
            {
                return false;
            }

            bool changed = false;

            {
                std::unique_lock lock(
                    nulledTvEffectPropertiesMutex
                );

                changed =
                    nulledTvEffectProperties.emplace(
                        shaderProperty
                    ).second;
            }

            if (changed)
            {
                REX::INFO(
                    "f4ffmpeg target-local workshop-TV {} property={} will return a null base texture.",
                    workshopTvEffectKindName(kind),
                    static_cast<const void*>(shaderProperty)
                );
            }

            return true;
        }

        bool isWorkshopTvEffectTextureNulled(
            const RE::BSShaderProperty* shaderProperty)
        {
            if (shaderProperty == nullptr)
                return false;

            std::shared_lock lock(
                nulledTvEffectPropertiesMutex
            );

            return nulledTvEffectProperties.find(
                       shaderProperty
                   ) != nulledTvEffectProperties.end();
        }

        RE::NiTexture* effectGetBaseTextureHook(
            const RE::BSShaderProperty* shaderProperty)
        {
            if (!effectBaseTextureHookObserved.exchange(
                    true,
                    std::memory_order_acq_rel
                ))
            {
                REX::INFO(
                    "f4ffmpeg BSEffectShaderProperty::GetBaseTexture hook reached."
                );
            }

            auto* baseTexture =
                originalEffectGetBaseTexture(
                    shaderProperty
                );

            if (
                shaderProperty != nullptr &&
                isWorkshopTvEffectTextureNulled(shaderProperty))
            {
                // Properties only enter the null set after GetRenderPasses has
                // proven they belong to a touched TV (fadeNode or scene ancestry).
                // GetBaseTexture has no geometry argument, so the property-local
                // authorization itself is the durable ownership proof here.
                return nullptr;
            }

            return baseTexture;
        }

        RE::BSShaderProperty::RenderPassArray*
        effectGetRenderPassesHook(
            RE::BSShaderProperty* shaderProperty,
            RE::BSGeometry* geometry,
            std::uint32_t renderMode,
            RE::BSShaderAccumulator* accumulator)
        {
            if (!effectRenderPassHookObserved.exchange(
                    true,
                    std::memory_order_acq_rel
                ))
            {
                REX::INFO(
                    "f4ffmpeg BSEffectShaderProperty::GetRenderPasses "
                    "hook reached."
                );
            }

            if (shaderProperty == nullptr)
            {
                return originalEffectGetRenderPasses(
                    shaderProperty,
                    geometry,
                    renderMode,
                    accumulator
                );
            }

            auto* baseTexture =
                originalEffectGetBaseTexture(
                    shaderProperty
                );

            const char* textureName =
                baseTexture != nullptr
                    ? baseTexture->name.c_str()
                    : nullptr;

            std::shared_ptr<const videoTarget>
                target;

            if (
                textureName != nullptr &&
                *textureName != '\0')
            {
                target =
                    getVideoTargetForTexture(
                        textureName
                    );

                if (target)
                {
                    // The DLC04 Nuka commercial is authored as a UV flipbook. A
                    // video SRV must see the full 0..1 UV range instead of one
                    // 1/16 x 1/16 atlas cell. The NIF bakes that cell into the
                    // BSTriShape vertex UVs, so after detaching the offset
                    // controllers we magnify the shader UV transform by 16.
                    // Apply this whenever the exact Nuka screen texture
                    // is actually resolved to an f4ffmpeg target, including
                    // Bethesda-placed Nuka-World FX references that were never
                    // created through our spawn path.
                    if (isNukaColaCommercialScreenTexture(textureName) &&
                        nukaColaCommercialSurfaceNeedsSanitization(
                            shaderProperty,
                            geometry
                        ))
                    {
                        // Load3D handles native surfaces that load while this
                        // replacement is already active. This render-time pass is
                        // the late-activation fallback: if the surface was loaded
                        // before a location-specific target became active, climb
                        // to the NIF root and run the same full sanitation now.
                        RE::NiAVObject* root = geometry;
                        while (root != nullptr && root->parent != nullptr)
                            root = root->parent;

                        nukaColaSanitizationStats stats{};
                        sanitizeNukaColaMachineScreenObject(
                            root,
                            stats,
                            true
                        );

                        if (stats.objectControllerChainsCleared != 0 ||
                            stats.propertyControllerChainsCleared != 0 ||
                            stats.behaviorGraphsRemoved != 0 ||
                            stats.videoUvMaterialsExpanded != 0 ||
                            stats.videoUvMaterialExpansionFailures != 0)
                        {
                            REX::INFO(
                                "f4ffmpeg actively sanitized swapped Nuka-Cola commercial geometry '{}': "
                                "{} object controller chain(s), {} property controller chain(s), "
                                "{} behavior graph(s) removed, {} UV material(s) atlas-expanded, "
                                "{} UV expansion failure(s).",
                                geometry != nullptr && geometry->name.c_str() != nullptr
                                    ? geometry->name.c_str()
                                    : "<unnamed>",
                                stats.objectControllerChainsCleared,
                                stats.propertyControllerChainsCleared,
                                stats.behaviorGraphsRemoved,
                                stats.videoUvMaterialsExpanded,
                                stats.videoUvMaterialExpansionFailures
                            );
                        }
                    }

                    // This rendered screen is now proven to be one f4ffmpeg
                    // handles. Record both ownership forms: fadeNode for the fast
                    // path and a shallow parent chain for TV assets whose sibling
                    // effect properties use different fade nodes.
                    markTvNodeTouched(
                        shaderProperty
                    );

                    markTvGeometryTouched(
                        geometry
                    );
                }
            }

            const bool touched =
                isTvInstanceTouched(
                    shaderProperty,
                    geometry
                );

            workshopTvEffectKind effectKind =
                workshopTvEffectKind::other;

            if (touched)
            {
                effectKind =
                    classifyWorkshopTvEffect(
                        shaderProperty,
                        geometry,
                        baseTexture
                    );

                diagnoseTouchedTvEffect(
                    shaderProperty,
                    geometry,
                    baseTexture,
                    effectKind
                );

                // Never classify the actual handled screen texture as an effect
                // to remove, even if a mod gives its geometry/material a name
                // containing one of our diagnostic tokens. For sibling TV effects
                // with the corresponding option enabled, deliberately use both
                // suppression boundaries:
                //
                //  1. mark the property so virtual GetBaseTexture returns nullptr;
                //  2. clear/return its engine-owned RenderPassArray before Bethesda
                //     can construct or submit the effect draw.
                //
                // RasterScan keeps its existing SRV-null presentation fallback as
                // a third layer. Register that binding before the early return.
                if (
                    !target &&
                    shouldSuppressWorkshopTvEffect(
                        effectKind
                    ))
                {
                    markWorkshopTvEffectTextureNull(
                        shaderProperty,
                        effectKind
                    );

                    if (
                        effectKind == workshopTvEffectKind::rasterScan &&
                        textureName != nullptr &&
                        *textureName != '\0')
                    {
                        registerNiTextureSuppressionBinding(
                            textureName,
                            baseTexture,
                            "BSEffectShaderProperty::GetRenderPasses"
                        );
                    }

                    shaderProperty->DoClearRenderPasses();

                    REX::DEBUG(
                        "f4ffmpeg target-local workshop-TV {} property={} blocked render-pass after nulling base-texture",
                        workshopTvEffectKindName(effectKind),
                        static_cast<const void*>(shaderProperty)
                    );

                    return std::addressof(
                        shaderProperty->renderPassList
                    );
                }
            }

            // Only unsuppressed effects reach Bethesda's pass builder.
            auto* passes =
                originalEffectGetRenderPasses(
                    shaderProperty,
                    geometry,
                    renderMode,
                    accumulator
                );

            if (
                textureName == nullptr ||
                *textureName == '\0')
            {
                return passes;
            }

            diagnoseResolvedTexturePath(
                "BSEffectShaderProperty::GetRenderPasses",
                textureName
            );

            if (target)
            {
                registerNiTexturePresentationBinding(
                    textureName,
                    baseTexture,
                    "BSEffectShaderProperty::GetRenderPasses"
                );
            }

            return passes;
        }

        bool installEffectTextureHook()
        {
            const auto vtable =
                findEffectShaderPropertyVtable();

            if (!vtable)
            {
                REX::ERROR(
                    "Failed to locate Fallout's BSEffectShaderProperty vtable; "
                    "effect/flipbook texture replacement is unavailable."
                );

                return false;
            }

            const auto getRenderPassesSlot =
                *vtable +
                sizeof(std::uintptr_t) *
                    effectGetRenderPassesVtableIndex;

            const auto getBaseTextureSlot =
                *vtable +
                sizeof(std::uintptr_t) *
                    effectGetBaseTextureVtableIndex;

            const auto renderPassesOriginal =
                readUnaligned<std::uintptr_t>(
                    getRenderPassesSlot
                );

            const auto baseTextureOriginal =
                readUnaligned<std::uintptr_t>(
                    getBaseTextureSlot
                );

            if (
                renderPassesOriginal == 0 ||
                baseTextureOriginal == 0)
            {
                return false;
            }

            originalEffectGetRenderPasses =
                renderPassesOriginal;

            originalEffectGetBaseTexture =
                baseTextureOriginal;

            const auto renderPassesReplacement =
                reinterpret_cast<std::uintptr_t>(
                    &effectGetRenderPassesHook
                );

            const auto baseTextureReplacement =
                reinterpret_cast<std::uintptr_t>(
                    &effectGetBaseTextureHook
                );

            REX::INFO(
                "f4ffmpeg BSEffectShaderProperty vtable={}; "
                "GetRenderPasses slot 2B={}, GetBaseTexture slot 39={}.",
                reinterpret_cast<void*>(
                    *vtable
                ),
                reinterpret_cast<void*>(
                    renderPassesOriginal
                ),
                reinterpret_cast<void*>(
                    baseTextureOriginal
                )
            );

            REL::Relocation<std::uintptr_t> effectVtable{
                *vtable
            };

            effectVtable.write_vfunc(
                effectGetRenderPassesVtableIndex,
                renderPassesReplacement
            );

            effectVtable.write_vfunc(
                effectGetBaseTextureVtableIndex,
                baseTextureReplacement
            );

            const auto patchedRenderPasses =
                readUnaligned<std::uintptr_t>(
                    getRenderPassesSlot
                );

            const auto patchedBaseTexture =
                readUnaligned<std::uintptr_t>(
                    getBaseTextureSlot
                );

            if (patchedRenderPasses != renderPassesReplacement)
            {
                REX::ERROR(
                    "Failed to patch BSEffectShaderProperty::GetRenderPasses."
                );

                return false;
            }

            if (patchedBaseTexture != baseTextureReplacement)
            {
                REX::ERROR(
                    "Failed to patch BSEffectShaderProperty::GetBaseTexture."
                );

                return false;
            }

            REX::INFO(
                "f4ffmpeg patched BSEffectShaderProperty "
                "GetRenderPasses slot 2B={} and GetBaseTexture slot 39={}.",
                reinterpret_cast<void*>(
                    patchedRenderPasses
                ),
                reinterpret_cast<void*>(
                    patchedBaseTexture
                )
            );

            REX::INFO(
                "f4ffmpeg effect/flipbook base-texture interception initialized."
            );

            return true;
        }

        REX::W32::ID3D11ShaderResourceView* getProducedSrv(
            const producedFrame& frame)
        {
            // The decoder now creates the Fallout-device SRV together with the
            // texture. The producedFrame owns both COM references, and the
            // shared_ptr held by the presentation hook keeps them alive through
            // PSSetShaderResources. D3D11 retains its own reference once bound.
            return frame.resourceView;
        }

        using psSetShaderResources_t = void(*)(
            REX::W32::ID3D11DeviceContext*,
            std::uint32_t,
            std::uint32_t,
            REX::W32::ID3D11ShaderResourceView* const*
        );

        REL::Relocation<psSetShaderResources_t>
            originalPSSetShaderResources;

        REX::W32::ID3D11DeviceContext*
            falloutDeviceContext = nullptr;

        void psSetShaderResourcesHook(
            REX::W32::ID3D11DeviceContext* context,
            std::uint32_t startSlot,
            std::uint32_t numViews,
            REX::W32::ID3D11ShaderResourceView* const* views)
        {
            if (
                context == falloutDeviceContext &&
                !psHookObserved.exchange(
                    true,
                    std::memory_order_acq_rel
                ))
            {
                REX::INFO(
                    "f4ffmpeg D3D11 PSSetShaderResources hook reached."
                );
            }

            if (
                context != falloutDeviceContext ||
                views == nullptr ||
                numViews == 0 ||
                !hasPresentationBindings.load(
                    std::memory_order_acquire
                ))
            {
                originalPSSetShaderResources(
                    context,
                    startSlot,
                    numViews,
                    views
                );

                return;
            }

            if (
                numViews >
                    REX::W32::D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
            {
                originalPSSetShaderResources(
                    context,
                    startSlot,
                    numViews,
                    views
                );

                return;
            }

            std::array<
                REX::W32::ID3D11ShaderResourceView*,
                REX::W32::D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT>
                replacementViews{};

            std::copy_n(
                views,
                numViews,
                replacementViews.begin()
            );

            bool replacedAny = false;
            bool presentedVideoFrame = false;

            for (
                std::uint32_t index = 0;
                index < numViews;
                ++index)
            {
                auto* vanillaSrv =
                    views[index];

                if (vanillaSrv == nullptr)
                    continue;

                std::shared_ptr<const videoTarget>
                    target;
                std::string texturePath;

                bool suppress = false;

                {
                    std::shared_lock lock(
                        bindingMutex
                    );

                    suppress =
                        suppressedVanillaSrvs.find(
                            vanillaSrv
                        ) != suppressedVanillaSrvs.end();

                    if (!suppress)
                    {
                        if (const auto texturePathBinding =
                                texturePathsByVanillaSrv.find(
                                    vanillaSrv
                                );
                            texturePathBinding != texturePathsByVanillaSrv.end())
                        {
                            texturePath =
                                texturePathBinding->second;
                        }

                        const auto binding =
                            targetsByVanillaSrv.find(
                                vanillaSrv
                            );

                        if (binding != targetsByVanillaSrv.end())
                        {
                            target =
                                binding->second;
                        }
                    }
                }

                if (suppress)
                {
                    // A null D3D11 SRV is a legal unbound resource. Shader reads
                    // return zero, removing the target-local RasterScan texture
                    // without manufacturing a replacement resource.
                    replacementViews[index] =
                        nullptr;

                    replacedAny = true;

                    if (!rasterScanSuppressionObserved.exchange(
                            true,
                            std::memory_order_acq_rel
                        ))
                    {
                        REX::INFO(
                            "f4ffmpeg target-local workshop-TV raster-scan suppression reached D3D11 presentation."
                        );
                    }

                    continue;
                }

                if (texturePath.empty())
                {
                    if (!target)
                        continue;

                    texturePath = target->getTexturePath();
                }

                // A vanilla SRV is shared by every instance of a texture. Resolve
                // again at draw time so a player location change selects that
                // location's manager instead of the target first seen on load.
                const auto locationTarget = getVideoTargetForTexture(
                    texturePath.c_str()
                );
                if (!locationTarget)
                {
                    // This may be a location-only definition. When the player
                    // leaves every configured location, preserve Fallout's
                    // supplied texture instead of the last location's video.
                    continue;
                }

                target = locationTarget;

                const auto transition =
                    target->getTransitionPresentation();

                if (transition.active)
                {
                    switch (transition.method)
                    {
                        case transitionMethod::vanillaDDS:
                            // Deliberately leave Bethesda's exact SRV in place.
                            // No synthetic fallback resource is created.
                            continue;

                        case transitionMethod::blackFrame:
                            // A null SRV is a legal D3D11 unbound resource and
                            // samples as zero. This is intentionally the same
                            // mechanism used by target-local raster suppression.
                            replacementViews[index] = nullptr;
                            replacedAny = true;
                            continue;

                        case transitionMethod::holdLastFrame:
                        case transitionMethod::image:
                        {
                            const auto& transitionFrame =
                                transition.frame;

                            if (
                                !transitionFrame ||
                                transitionFrame->texture == nullptr ||
                                transitionFrame->resourceView == nullptr)
                            {
                                // HoldLastFrame has nothing useful to hold before
                                // the first produced frame. Image is resolved back
                                // to the global fallback before reaching this path
                                // if its preload failed. Vanilla is the safest
                                // final presentation fallback.
                                continue;
                            }

                            auto* transitionSrv =
                                getProducedSrv(
                                    *transitionFrame
                                );

                            if (transitionSrv == nullptr)
                                continue;

                            replacementViews[index] =
                                transitionSrv;

                            replacedAny = true;
                            continue;
                        }
                    }
                }

                const auto frame =
                    target->getLatestFrame();

                if (
                    !frame ||
                    frame->texture == nullptr ||
                    frame->resourceView == nullptr)
                {
                    // No produced frame: use the exact vanilla SRV Fallout
                    // supplied. This is the normal fallback path.
                    continue;
                }

                auto* producedSrv =
                    getProducedSrv(
                        *frame
                    );

                if (producedSrv == nullptr)
                    continue;

                replacementViews[index] =
                    producedSrv;

                replacedAny = true;
                presentedVideoFrame = true;
            }

            if (
                presentedVideoFrame &&
                !presentationObserved.exchange(
                    true,
                    std::memory_order_acq_rel
                ))
            {
                REX::INFO(
                    "f4ffmpeg D3D11 produced-frame presentation reached."
                );
            }

            originalPSSetShaderResources(
                context,
                startSlot,
                numViews,
                replacedAny
                    ? replacementViews.data()
                    : views
            );
        }

        bool installPresentationHook()
        {
            auto* rendererData =
                RE::BSGraphics::GetRendererData();

            if (
                rendererData == nullptr ||
                rendererData->context == nullptr)
            {
                REX::ERROR(
                    "Cannot install f4ffmpeg presentation hook: "
                    "Fallout D3D11 context is unavailable."
                );

                return false;
            }

            falloutDeviceContext =
                rendererData->context;

            const auto vtableAddress =
                *reinterpret_cast<std::uintptr_t*>(
                    falloutDeviceContext
                );

            if (vtableAddress == 0)
            {
                REX::ERROR(
                    "Cannot install f4ffmpeg presentation hook: "
                    "D3D11 context vtable is unavailable."
                );

                return false;
            }

            REL::Relocation<std::uintptr_t>
                vtable{
                    vtableAddress
                };

            const auto original =
                *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() +
                    sizeof(void*) *
                        psSetShaderResourcesVtableIndex
                );

            if (original == 0)
            {
                REX::ERROR(
                    "Cannot install f4ffmpeg presentation hook: "
                    "PSSetShaderResources target is unavailable."
                );

                return false;
            }

            originalPSSetShaderResources =
                original;

            REX::INFO(
                "f4ffmpeg D3D11 context vtable={}; "
                "PSSetShaderResources original={}.",
                reinterpret_cast<void*>(vtable.address()),
                reinterpret_cast<void*>(original)
            );

            vtable.write_vfunc(
                psSetShaderResourcesVtableIndex,
                psSetShaderResourcesHook
            );

            const auto patched =
                *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() +
                    sizeof(void*) *
                        psSetShaderResourcesVtableIndex
                );

            REX::INFO(
                "f4ffmpeg D3D11 PSSetShaderResources patched={}.",
                reinterpret_cast<void*>(patched)
            );

            REX::INFO(
                "f4ffmpeg D3D11 texture presentation hook initialized."
            );

            return true;
        }

        bool installTextureHooks()
        {
            REL::Relocation<std::uintptr_t>
                vtable{
                    RE::BSShaderTextureSet::VTABLE[0]
                };

            const auto filenameOriginal =
                *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() +
                    sizeof(void*) * 0x29
                );

            const auto prefetchedOriginal =
                *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() +
                    sizeof(void*) * 0x2A
                );

            const auto ordinaryOriginal =
                *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() +
                    sizeof(void*) * 0x2B
                );

            if (
                filenameOriginal == 0 ||
                prefetchedOriginal == 0 ||
                ordinaryOriginal == 0)
            {
                REX::ERROR(
                    "Failed to resolve f4ffmpeg BSShaderTextureSet "
                    "interception targets."
                );

                return false;
            }

            originalGetTextureFilename =
                filenameOriginal;

            originalGetTexturePrefetched =
                prefetchedOriginal;

            originalGetTexture =
                ordinaryOriginal;

            REX::INFO(
                "f4ffmpeg BSShaderTextureSet vtable={}; "
                "original slots 29={}, 2A={}, 2B={}.",
                reinterpret_cast<void*>(vtable.address()),
                reinterpret_cast<void*>(filenameOriginal),
                reinterpret_cast<void*>(prefetchedOriginal),
                reinterpret_cast<void*>(ordinaryOriginal)
            );

            vtable.write_vfunc(
                0x29,
                getTextureFilenameHook
            );

            vtable.write_vfunc(
                0x2A,
                getTexturePrefetchedHook
            );

            vtable.write_vfunc(
                0x2B,
                getTextureHook
            );

            const auto filenamePatched =
                *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() +
                    sizeof(void*) * 0x29
                );

            const auto prefetchedPatched =
                *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() +
                    sizeof(void*) * 0x2A
                );

            const auto ordinaryPatched =
                *reinterpret_cast<const std::uintptr_t*>(
                    vtable.address() +
                    sizeof(void*) * 0x2B
                );

            REX::INFO(
                "f4ffmpeg patched BSShaderTextureSet slots "
                "29={}, 2A={}, 2B={}.",
                reinterpret_cast<void*>(filenamePatched),
                reinterpret_cast<void*>(prefetchedPatched),
                reinterpret_cast<void*>(ordinaryPatched)
            );

            REX::INFO(
                "f4ffmpeg BSShaderTextureSet interception initialized."
            );

            return true;
        }
    }

    void videoTarget::attachPlayback(
        std::shared_ptr<manager> newPlayback)
    {
        std::unique_lock lock(
            playbackMutex
        );

        playback =
            std::move(newPlayback);
    }

    std::shared_ptr<const producedFrame>
    videoTarget::getLatestFrame() const
    {
        std::shared_ptr<manager> currentPlayback;

        {
            std::shared_lock lock(
                playbackMutex
            );

            currentPlayback =
                playback;
        }

        if (!currentPlayback)
            return nullptr;

        return currentPlayback->getLatestFrame();
    }


    transitionPresentation
    videoTarget::getTransitionPresentation() const
    {
        std::shared_ptr<manager> currentPlayback;

        {
            std::shared_lock lock(
                playbackMutex
            );

            currentPlayback =
                playback;
        }

        if (!currentPlayback)
            return {};

        return currentPlayback->getTransitionPresentation();
    }

    std::optional<std::string>
    getVideoPathForTexture(
        const char* texturePath)
    {
        const auto resolved =
            resolveVideoTarget(
                texturePath
            );

        if (!resolved)
            return std::nullopt;

        return resolved->videoPath;
    }

    std::shared_ptr<const videoTarget>
    getVideoTargetForTexture(
        const char* texturePath)
    {
        const auto resolved =
            resolveVideoTarget(
                texturePath
            );

        if (!resolved)
            return nullptr;

        std::string textureKey =
            lowercasePath(resolved->texturePath);
        textureKey += "|";
        textureKey += lowercasePath(resolved->playbackKey);

        ensurePlaybackActivationRecord(
            resolved->playbackKey,
            resolved->videoPath,
            resolved->playbackSettings
        );

        std::scoped_lock lock(
            targetRegistryMutex
        );

        if (const auto existing =
                targetsByTexture.find(
                    textureKey
                );
            existing != targetsByTexture.end())
        {
            if (!existing->second->activationRequestIssued.exchange(
                    true,
                    std::memory_order_acq_rel))
            {
                requestPlaybackActivation(
                    existing->second->playbackKey
                );
            }

            return existing->second;
        }

        // Target creation is cheap and decoder-free. The first mapped texture
        // request queues its playback definition for asynchronous activation;
        // until that manager is ready, Fallout keeps presenting the vanilla SRV.
        auto target =
            std::make_shared<videoTarget>();

        target->mode =
            resolved->mode;

        target->texturePath =
            resolved->texturePath;

        target->videoPath =
            resolved->videoPath;

        target->playbackKey =
            resolved->playbackKey;

        target->playbackSettings =
            resolved->playbackSettings;

        {
            const std::string playbackKey =
                lowercasePath(
                    resolved->playbackKey
                );

            std::scoped_lock playbackLock(
                playbackRegistryMutex
            );

            if (const auto playback =
                    playbackByIdentity.find(
                        playbackKey
                    );
                playback != playbackByIdentity.end())
            {
                target->playback =
                    playback->second;
            }
        }

        targetsByTexture.emplace(
            textureKey,
            target
        );

        target->activationRequestIssued.store(
            true,
            std::memory_order_release
        );

        requestPlaybackActivation(
            target->playbackKey
        );

        return target;
    }

    bool dispatchVideoManagers()
    {
        std::size_t indexedDefinitions = 0;

        {
            std::scoped_lock activationLock(
                activationRegistryMutex
            );

            indexedDefinitions =
                activationByIdentity.size();
        }

        std::size_t queuedDefinitions = 0;
        bool startedWorker = false;

        {
            std::scoped_lock queueLock(
                activationQueueMutex
            );

            activationEnabled = true;
            queuedDefinitions =
                activationQueue.size();

            if (!activationWorker.joinable())
            {
                activationStopRequested = false;
                activationWorker =
                    std::thread(activationWorkerMain);
                startedWorker = true;
            }
        }

        activationQueueCv.notify_all();

        REX::INFO(
            "f4ffmpeg post-load lazy manager activation armed: {} indexed playback definition(s), {} already requested/queued, worker {}.",
            indexedDefinitions,
            queuedDefinitions,
            startedWorker ? "started" : "already running"
        );

        return true;
    }

    void injectNukaColaMachineScreensForLoadedCell()
    {
        if (!nukaColaMachineScreenInjectionEnabled)
            return;

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player != nullptr
            ? player->GetParentCell()
            : nullptr;

        if (cell == nullptr)
        {
            REX::WARN(
                "f4ffmpeg could not scan Nuka-Cola screen targets after game load: the player cell is unavailable."
            );
            return;
        }

        injectNukaColaMachineScreensInCell(
            *cell,
            "PostLoadGame player cell"
        );
    }

    void resetNukaColaMachineScreenInjection()
    {
        std::scoped_lock lock(
            injectedNukaColaMachineReferencesMutex
        );
        injectedNukaColaMachineReferences.clear();
    }

    bool initializeNifHandler()
    {
        static std::once_flag initializeOnce;
        static bool initialized = false;

        std::call_once(
            initializeOnce,
            []()
            {
                auto* rendererData =
                    RE::BSGraphics::GetRendererData();

                if (
                    rendererData == nullptr ||
                    !rendererData->initialized ||
                    rendererData->device == nullptr ||
                    rendererData->context == nullptr)
                {
                    REX::ERROR(
                        "f4ffmpeg texture replacement initialization requires "
                        "Fallout graphics to be ready."
                    );

                    return;
                }

                REX::INFO(
                    "Initializing f4ffmpeg texture replacement."
                );

                workshopTvRasterScanSuppressionEnabled =
                    config::disableWorkshopTVRasterScan.GetValue();

                workshopTvStaticSuppressionEnabled =
                    config::disableWorkshopTVStatic.GetValue();

                workshopTvWarpSuppressionEnabled =
                    config::disableWorkshopTVWarp.GetValue();

                nukaColaMachineScreenInjectionEnabled =
                    config::enableNukaColaMachineScreens.GetValue();
                nukaColaMachineScreenSourceForm =
                    config::nukaColaMachineScreenSourceForm.GetValue();
                REX::INFO(
                    "f4ffmpeg Extra Nuka-Cola screen config: enabled={}, target forms={}, source form='{}'.",
                    nukaColaMachineScreenInjectionEnabled,
                    config::nukaColaMachineScreenTargets.GetValue().size(),
                    nukaColaMachineScreenSourceForm
                );

                for (const auto& editorId :
                     config::nukaColaMachineScreenTargets.GetValue())
                {
                    if (const auto formId = parseConfiguredFormId(editorId))
                    {
                        nukaColaMachineScreenTargetFormIds.emplace(*formId);
                    }
                    else if (resemblesFormIdSelector(editorId))
                    {
                        REX::WARN(
                            "f4ffmpeg ignored malformed Extra.NukaColaMachineScreenTargets selector '{}'; FormIDs must be exactly eight hexadecimal digits (for example 00034661 or 0x00034661).",
                            editorId
                        );
                    }
                    else if (!editorId.empty())
                    {
                        nukaColaMachineScreenTargets.emplace(
                            lowercasePath(editorId)
                        );
                    }
                }

                if (nukaColaMachineScreenInjectionEnabled &&
                    nukaColaMachineScreenTargets.empty() &&
                    nukaColaMachineScreenTargetFormIds.empty())
                {
                    REX::WARN(
                        "f4ffmpeg Nuka-Cola screen injection is enabled but Extra.NukaColaMachineScreenTargets is empty."
                    );
                    nukaColaMachineScreenInjectionEnabled = false;
                }
                else if (nukaColaMachineScreenInjectionEnabled)
                {
                    auto* cellEventSource =
                        RE::TESCellFullyLoadedEvent::GetEventSource();
                    if (nukaColaMachineScreenInjectionEnabled &&
                        cellEventSource == nullptr)
                    {
                        REX::WARN(
                            "f4ffmpeg could not register Nuka-Cola screen injection: cell event source is unavailable."
                        );
                    }
                    else if (nukaColaMachineScreenInjectionEnabled)
                    {
                        cellEventSource->RegisterSink(
                            std::addressof(nukaColaMachineCellEventSink)
                        );
                        REX::INFO(
                            "f4ffmpeg Nuka-Cola screen injection enabled for {} base form selector(s) ({} EditorID, {} FormID), source form='{}'.",
                            nukaColaMachineScreenTargets.size() +
                                nukaColaMachineScreenTargetFormIds.size(),
                            nukaColaMachineScreenTargets.size(),
                            nukaColaMachineScreenTargetFormIds.size(),
                            nukaColaMachineScreenSourceForm
                        );
                    }
                }

                REX::INFO(
                    "f4ffmpeg target-local workshop-TV effects: raster scan={}, static/fuzz={}, warp={}.",
                    workshopTvRasterScanSuppressionEnabled
                        ? "disabled"
                        : "preserved",
                    workshopTvStaticSuppressionEnabled
                        ? "disabled"
                        : "preserved",
                    workshopTvWarpSuppressionEnabled
                        ? "disabled"
                        : "preserved"
                );

                if (!buildReplacementIndex())
                {
                    REX::WARN(
                        "f4ffmpeg replacement index encountered filesystem "
                        "errors; successfully indexed entries remain usable."
                    );
                }

                // Install after indexing. Sanitization is enabled only when the
                // exact Nuka commercial texture is indexed, and each Load3D call
                // still checks whether that replacement resolves in the current
                // location/worldspace before touching the reference.
                if (!installNukaColaActiveReplacementSanitizationHook())
                {
                    REX::WARN(
                        "f4ffmpeg could not install Nuka-Cola Load3D sanitation; "
                        "render-time active-swap sanitation remains available."
                    );
                }

                if (!installPresentationHook())
                    return;

                if (!installTextureHooks())
                    return;

                if (!installEffectTextureHook())
                {
                    REX::WARN(
                        "f4ffmpeg effect/flipbook interception could not be "
                        "installed; BSShaderTextureSet replacement remains active."
                    );
                }

                initialized = true;

                REX::INFO(
                    "f4ffmpeg texture replacement initialized."
                );
            }
        );

        return initialized;
    }
}
