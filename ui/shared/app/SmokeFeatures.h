#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "tesseract/types.h"

namespace tesseract::smoke
{

/// One entry of the smoke `features` manifest. `args` is the whole check
/// object, so later steps can read `room_id`, `event_id`, `expect`, `op`, ...
struct FeatureCheck
{
    std::string name;
    /// "event", "room", "action" or "order".
    std::string kind;
    nlohmann::json args;
};

/// Parses `{"checks": [{"name", "kind", ...}, ...]}`. Returns empty and sets
/// `error` on malformed input, an unknown kind or an empty `checks` array (an
/// empty manifest would pass the phase with nothing checked).
std::vector<FeatureCheck> parse_feature_manifest(const std::string& json_text, std::string& error);

/// The manifest's top-level `features_room_id`, or empty when absent.
std::string manifest_features_room_id(const std::string& json_text);

/// Empty when every key of `expect` matches `ev`, else a text such as
/// "is_edited: want true, got false". An unknown key is a mismatch.
std::string event_mismatch(const Event& ev, const nlohmann::json& expect);

/// For a check that expects `{"absent": true}`: empty when the event id was
/// never recorded, else a failure text.
std::string absent_mismatch(bool recorded);

/// Same contract as event_mismatch, for `RoomInfo`.
std::string room_mismatch(const RoomInfo& room, const nlohmann::json& expect);

} // namespace tesseract::smoke
