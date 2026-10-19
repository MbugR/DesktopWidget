#pragma once

// "Start with Windows". The app runs elevated (requireAdministrator) so the
// PawnIO sensors (CPU temperature, motherboard fans) work. A plain HKCU Run
// value cannot launch an elevated app at logon without a UAC prompt, so
// autostart is a Task Scheduler task ("DesktopWidget") with a logon trigger
// for the current user and highest run level, giving an elevated launch at
// logon with no prompt. Creating or deleting the task needs admin, which the
// elevated app already has. Versions that used an HKCU Run value are migrated
// off it (the old value is deleted) when autostart is enabled.

// True when the task exists and its action points at this exe.
bool Autostart_IsEnabled();

// Creates/updates (enable) or deletes (disable) the task. Returns false on
// error. Enabling also removes the legacy HKCU Run value if present.
bool Autostart_SetEnabled(bool enable);

// When enabled but the task's action path is stale (the exe moved), rewrites
// it to the current path. No-op when disabled or already current. Call at
// startup. Returns false when autostart is off or on error.
bool Autostart_RefreshPath();

// Call once at startup. If a previous version left an HKCU Run value, creates
// the task and removes that value (migration). Otherwise, if the task exists
// with a stale path, refreshes it to the current exe.
void Autostart_StartupSync();
