from pathlib import Path
import re
import sys


def replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise ValueError(f"Expected exactly one match for {old[:80]!r}")
    return text.replace(old, new, 1)


def regex_once(text: str, pattern: str, new: str) -> str:
    updated, count = re.subn(pattern, lambda _: new, text, flags=re.S)
    if count != 1:
        raise ValueError(f"Expected exactly one match for {pattern!r}, got {count}")
    return updated


def repair(text: str) -> str:
    text = replace_once(text, 'REX::LOG_DEBUG("URL deferred: %s", entry);', 'REX::DEBUG("URL deferred: {}", entry);')
    text = replace_once(text, r'''                    REX::LOG_DEBUG(
                        "  [INI %s line %d]: parsing INI item "%s"",
                        iniPath.string().c_str(), lineNumber, value.c_str()
                    );''', r'''                    REX::DEBUG(
                        "  [INI {} line {}]: parsing INI item \"{}\"",
                        iniPath.string(), lineNumber, value
                    );''')
    text = regex_once(text,
        r'                    const auto& locationSettings = \*overrideIt;.*?                    result\.locationKey = locId;',
        '''                    const auto& locationSettings = *overrideIt;
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
                    result.locationKey = locationKey;''')
    text = regex_once(text,
        r'                REX::LOG_DEBUG\(\n                    "  f4ffmpeg INDEXING standalone INI.*?\n                \);\n', '')
    text = replace_once(text,
        '            if (replacement->second.standalonePlaylist &&\n',
        '            std::string videoPath = replacement->second.videoPath;\n\n            if (replacement->second.standalonePlaylist &&\n')
    text = replace_once(text,
        '''REX::LOG_DEBUG("  -> using global playlist: %s (standalone INI always uses its own playlist)",
                             replacement->second.playbackSettings.playlist.front().c_str());''',
        '''REX::DEBUG("  -> using global playlist: {} (standalone INI always uses its own playlist)",
                             replacement->second.playbackSettings.playlist.front());''')
    text = replace_once(text,
        '''REX::LOG_DEBUG(
                        "f4ffmpeg target-local workshop-TV %s property=%p blocked render-pass after nulling base-texture",
                        workshopTvEffectKindName(effectKind).c_str(),
                        static_cast<void*>(shaderProperty)
                    );''',
        '''REX::DEBUG(
                        "f4ffmpeg target-local workshop-TV {} property={} blocked render-pass after nulling base-texture",
                        workshopTvEffectKindName(effectKind),
                        static_cast<const void*>(shaderProperty)
                    );''')
    if "REX::LOG_" in text:
        raise ValueError("Unsupported REX logging entrypoint remains")
    return text


if __name__ == "__main__":
    target = Path(sys.argv[1]) / "src/nifHandler.cpp"
    original = target.read_bytes()
    if b"\r\n" in original:
        raise ValueError("Expected LF source bytes")
    target.write_bytes(repair(original.decode("utf-8")).encode("utf-8"))
