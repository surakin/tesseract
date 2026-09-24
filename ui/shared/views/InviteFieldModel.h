#pragma once

// Pure (host-free) parser for InviteDialog's search field. The field is a
// pill-capable tk::TextArea whose content mixes mention pills (the users
// selected for invitation) with typed text (a live filter and/or full
// "@user:server" mxids). This turns the native text() + composer_draft()
// pair into byte-addressed pieces the dialog can act on:
//
//   * pills     — one per mention pill, in document order, with the byte
//                 offset of its U+FFFC placeholder in text().
//   * committed — complete-mxid tokens that are finished (not under the
//                 caret), i.e. followed by a separator or typed/pasted
//                 before other content. The dialog turns these into pills.
//   * query     — the token ending at the caret (may be empty): the live
//                 filter, never auto-converted (the user may still be
//                 typing "@bob:matrix.o…").
//
// Separators are ASCII whitespace, ',' and ';'. A pill placeholder also
// ends a token.

#include <tesseract/mentions.h>

#include <string>
#include <string_view>
#include <vector>

namespace tesseract::views
{

struct InviteFieldPill
{
    int         start = 0; // byte offset of the U+FFFC placeholder
    std::string user_id;
    std::string display_name;
};

struct InviteFieldToken
{
    int         start = 0; // byte range [start, end) in text()
    int         end   = 0;
    std::string text;
};

struct InviteFieldParse
{
    std::vector<InviteFieldPill>  pills;
    std::vector<InviteFieldToken> committed;
    InviteFieldToken              query;
};

// Byte length of a pill placeholder (U+FFFC) in text().
inline constexpr int kInvitePillBytes = 3;

// `text` is the native text() (pills as U+FFFC), `segs` its composer_draft()
// (the Nth Mention segment describes the Nth placeholder), `cursor` the
// caret's byte offset (cursor_byte_pos()); a caret outside [0, size] is
// treated as end-of-text.
InviteFieldParse parse_invite_field(std::string_view text,
                                    const std::vector<tesseract::MentionSeg>& segs,
                                    int cursor);

// A complete mxid: "@localpart:server" with both parts non-empty and no
// whitespace. Loose by design (the homeserver is the authority) — matches
// the quick switcher's user-mode test.
bool is_complete_mxid(std::string_view s);

} // namespace tesseract::views
