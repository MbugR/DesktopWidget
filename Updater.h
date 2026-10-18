#pragma once

// Manual update from the GitHub release ("Check for updates" in the tray).
// Nothing runs on its own: no check at startup, no timer.

// Asks GitHub for the latest build on a worker thread and reports through
// message boxes. When the user accepts an update, the new exe is downloaded
// next to ours as "<Name>.exe.new" and SHA-256 checked, a hidden PowerShell
// helper waits for this process to exit, swaps the file in and starts it,
// and then `requestExit` is called (on the worker thread): it must make the
// app quit through its normal shutdown path. A click while a check is still
// running is ignored.
void Updater_CheckAsync(void (*requestExit)());
