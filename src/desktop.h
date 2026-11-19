/* desktop.h - finding the spot behind the desktop icons, and checking
 * whether anybody can see it. */
#pragma once
#include "common.h"

/* Asks Explorer to split the wallpaper from the icon layer and returns the
   window to parent ours to. *icons is set to the icon layer when it is a
   sibling we must stay directly below (Windows 11 24H2+), else NULL. */
HWND desktop_host(HWND *icons);

/* TRUE when other windows cover every pixel of every monitor. */
BOOL desktop_covered(HWND self);

/* TRUE when the screen point is over the desktop, not an app window. */
BOOL desktop_under(POINT pt);
