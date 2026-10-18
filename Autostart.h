#pragma once

// "Start with Windows", via the per-user HKCU Run key with the quoted path of
// this exe. No elevation is needed to set it, and the app itself runs fine
// without admin -- only the PawnIO sensors (CPU temperature, motherboard
// fans) need admin, and they just show no value without it. A launch at
// logon therefore stays non-elevated; the README describes a scheduled task
// for users who want the PawnIO sensors to work at logon.

// True when the Run value points at this exe.
bool Autostart_IsEnabled();

// Adds (enable) or removes (disable) the Run value. Returns false on a
// registry error.
bool Autostart_SetEnabled(bool enable);
