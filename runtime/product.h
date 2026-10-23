// New in BotW Recomp (based on LAS Recomp by Peiikai, MIT).
// The product's names: the executable, the game-code library, the settings file and the name shown to the player.
// The Makefile sets them per game (GAME=botw: BotW Recomp; GAME=las and the default: LAS Recomp's own names, as
// upstream has them), so a Link's Awakening build is named exactly as before.
#pragma once

#ifndef PRODUCT_NAME
#define PRODUCT_NAME "LAS Recomp"            // window title, message boxes, start screen
#endif
#ifndef PRODUCT_TITLE
#define PRODUCT_TITLE "LAS Recompiled"       // the start screen's heading
#endif
#ifndef PRODUCT_EXE
#define PRODUCT_EXE "lasrecomp"              // the executable (and its import library, log file, Linux data folder)
#endif
#ifndef PRODUCT_GAMELIB
#define PRODUCT_GAMELIB "lasgame"            // the game-code library, without its extension
#endif
#ifndef PRODUCT_SETTINGS
#define PRODUCT_SETTINGS "larecomp.ini"      // the player's settings
#endif
