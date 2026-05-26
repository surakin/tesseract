# What's next?

Prepare the app to support translating every user-facing text.

Add desktop icon to the arch package

Leave chip buttons visible while the reaction emoji picker is open

'Add to saved stickers' should check if the sticker is in any of the subscribed sticker packs (using the mxc uri) too to decide if it can be saved or not.

https://github.com/matrix-org/matrix-spec-proposals/blob/main/proposals/3381-polls.md
https://github.com/matrix-org/matrix-spec-proposals/blob/main/proposals/4133-extended-profiles.md
https://github.com/matrix-org/matrix-spec-proposals/blob/main/proposals/4175-profile-field-time-zone.md

MSC4027 — custom emoji in reactions (the obvious next step users will want) # DONE
MSC4389 — image ordering within packs (gomuks already implements this experimentally)
MSC4377 — clarifications on pack ordering
MSC4459 — discovering image packs by clicking an emoji/sticker in a message

Handle server going offline gracefully

Threads? Backend is there, UI is missing.
How to do proper in-app versioning?
Add more things to the BrandView
Pinned messages!
Clear cache button?

Presence polling could be further narrowed to only the DM counterparts currently visible in the room list viewport (window-hidden suspension already lands in the unreleased changes)

The room list is misbehaving. Read the code and make it follow these rules:
The room list is composed of several sections with different behaviors.

1) The invite section. It only shows up when there is an active invitation, otherwise it's hidden.
2) The favorites section. All the rooms marked as favorites will show up here, even if they are inside a space.
3) The DMs section. Every room marked as a direct message will be here.
4) The rooms section. Every other room that is not inside a space will be here.
5) The spaces section. This is a hierarchical section. Initially only root spaces are here, but it follows the drill navigation model: clicking on a space will make the room list display the favorites, DMs, rooms and spaces that are INSIDE the selected space. The space header widget, above the room list, becomes visible, showing the current space name. The header back button allows the user to navigate back to the parent space (or no space)
6) The inactive section. Rooms with their last event timestamp older than the configurable inactivity period will be grouped here (if the group inactive rooms setting is enabled)

Every room displays a preview of the last event, if it's text, an image, or a sticker. This last event will be used to decide if the room goes in the inactive section or not. This is not received on the initial sync, so it has to be requested. This means fetching the last events for every single room, which can take a very long time. 