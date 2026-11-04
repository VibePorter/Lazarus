// Mass Effect 2 - saves in the Switch's own save data (see me2_switch_save.cpp).
#pragma once

namespace rex {
class Runtime;
}

namespace me2::switch_save {

// Before the game starts: picks the user, mounts (creating if needed) that user's save data of the forwarder's title,
// copies the SD saves in on the first start, and points the content store's saved games there. Does nothing on the
// PC, from hbmenu / title takeover, or with me2_switch_save = false (the saves stay on the SD card).
void Setup(rex::Runtime* runtime);

// A commit that failed (a save file was still open for writing) is tried again; called when the game loses focus
// (HOME menu), where it may be closed.
void CommitPending();

void Shutdown();

}  // namespace me2::switch_save
