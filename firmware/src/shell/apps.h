#pragma once
#include "app.h"

// The app registry — every app compiled into the firmware, in launcher order.
// Adding an app means writing its AppDef and adding one line to apps.cpp.
int           apps_count(void);
const AppDef* apps_at(int i);
