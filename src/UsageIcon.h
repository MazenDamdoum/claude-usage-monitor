#pragma once
#include <windows.h>

// Ring gauge icon: the arc fills clockwise from 12 o'clock to `percent`
// (-1 = unknown, grey ring only). severity: 0 normal, 1 warning, 2 critical.
HICON CreateUsageIcon(int size, int percent, int severity);
