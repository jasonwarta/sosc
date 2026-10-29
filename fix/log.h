// Session logging for streets_fix.dll.
#pragma once
#include <windows.h>

// Opens <game_dir>\logs\session_<date>_<time>.log, keeps the newest LOG_KEEP sessions,
// routes the game's stdout/stderr into the same file and installs the fault logger.
void log_init(const char *game_dir);
void logmsg(const char *fmt, ...);
const char *log_path(void);
