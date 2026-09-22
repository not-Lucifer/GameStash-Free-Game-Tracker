// Resource IDs shared between app.rc and main.cpp.
//
// IDI_APPICON is deliberately 1: Explorer and the shell show the
// LOWEST-numbered icon resource in a binary, so keeping this at 1
// guarantees the app icon is the one used for the .exe, shortcuts
// and the installer's "Apps & features" entry.
#pragma once

#define IDI_APPICON 1
