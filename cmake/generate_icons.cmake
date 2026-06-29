# Embed Lucide SVGs as C byte arrays into icons.h.
# Called at build time via add_custom_command so the header is rebuilt
# whenever a source SVG changes.
# Required -D arguments: LUCIDE_DIR, ICONS_H_OUT

set(_ci "#pragma once\n#include <cstdint>\n\n")
foreach(_entry
        "laugh|kEmojiSvg" "sticker|kStickerSvg" "mic|kMicSvg" "stop|kVoiceStopSvg"
        "reply|kReplySvg" "thread|kThreadSvg" "edit|kEditSvg" "more|kMoreSvg"
        "redact|kRedactSvg" "pin|kPinSvg" "download|kDownloadSvg" "close|kCloseSvg"
        "play|kPlaySvg" "join|kJoinSvg" "jump-to-date|kJumpToDateSvg"
        "threadlist|kThreadListSvg" "search|kSearchSvg" "chevron-up|kChevronUpSvg"
        "chevron-down|kChevronDownSvg" "forward|kForwardSvg"
        "arrow-left|kArrowLeftSvg"
        "phone|kPhoneSvg" "phone-off|kPhoneOffSvg" "mic-off|kMicOffSvg"
        "video|kVideoSvg" "video-off|kVideoOffSvg"
        "expand|kExpandSvg" "minimize|kMinimizeSvg" "pip|kPipSvg"
        "monitor|kMonitorSvg")
    string(REPLACE "|" ";" _pair "${_entry}")
    list(GET _pair 0 _name)
    list(GET _pair 1 _var)
    file(READ "${LUCIDE_DIR}/lucide-${_name}.svg" _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " _bytes "${_hex}")
    string(APPEND _ci "static constexpr std::uint8_t ${_var}[] = { ${_bytes}};\n")
endforeach()
file(WRITE "${ICONS_H_OUT}" "${_ci}")
