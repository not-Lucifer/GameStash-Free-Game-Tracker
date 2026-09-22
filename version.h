// Single source of truth for the Game Stash version.
//
// app.rc (the exe's version resource), main.cpp (the update check),
// build_dist.bat (the dist folder / zip name) and installer.iss (via the
// built exe's ProductVersion) all read this file. To release: bump the
// numbers here, build, and publish a GitHub release tagged vMAJOR.MINOR.PATCH
// with GameStash-MAJOR.MINOR.PATCH-Setup.exe attached.
//
// The string forms must match the numbers; rc.exe cannot reliably stringize
// macros, so they are spelled out.
#pragma once

#define GS_VERSION_MAJOR 1
#define GS_VERSION_MINOR 0
#define GS_VERSION_PATCH 1
#define GS_VERSION_STRING  "1.0.2"
#define GS_VERSION_STRING4 "1.0.2.0"
