Prepare the app to support translating every user-facing text.

add desktop icon to the arch package
make sure only one instance of the application can run at the same time

Leave chip buttons visible while the reaction emoji picker is open

'Add to saved stickers' should check if the sticker is in any of the subscribed sticker packs (using the mxc uri) too to decide if it can be saved or not.
In the emoji picker, the emojis are not properly centered on their cells.

Design the ability of opening a room in a separate separate room-only window (with its own message list and composer bar) Add a context menu to the room list with this single option for now.

-
The typing notification bar should be part of the message row list; if the user scrolls up, it should not be visible.

The image viewer is not opening the image at 1:1 scale.

The desktop notification is showing the mxid instead of the display name. See if it's possible to show the room display name and the sender display name.
-
When I open the image viewer it adds a transparent background (which is correct) but as soon as I move the mouse the background (around the image) turns black. See if the video player is doing the same, and fix both.

Clickable URL links are not clickable (neither they highlight on hover)

Design a 'brand view' with the app icon, app name, and current version to display when there is no active room in the main window.


Room list redesign: add a single line separator between rooms (not between sections). For each room row, next to the avatar it should display one line with the room display name (with a regular font, not bold) and another line with the last message received, if any, with a slightly smaller font. If no last message is known, show only the room name, vertically centered. Decrease the room row padding by half.

https://github.com/matrix-org/matrix-spec-proposals/blob/main/proposals/2010-spoilers.md
https://github.com/matrix-org/matrix-spec-proposals/blob/main/proposals/3030-jump-to-date.md
https://github.com/matrix-org/matrix-spec-proposals/blob/main/proposals/4230-animated-image-flag.md
https://github.com/matrix-org/matrix-spec-proposals/blob/main/proposals/3765-rich-room-topics.md
https://github.com/matrix-org/matrix-spec-proposals/blob/main/proposals/3266-room-summary.md

https://github.com/matrix-org/matrix-spec-proposals/blob/main/proposals/3381-polls.md
https://github.com/matrix-org/matrix-spec-proposals/blob/main/proposals/4133-extended-profiles.md
https://github.com/matrix-org/matrix-spec-proposals/blob/main/proposals/4175-profile-field-time-zone.md
