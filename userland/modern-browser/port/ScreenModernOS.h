#pragma once

namespace ModernOS {
// Display metadata comes from the desktop through the trusted broker. Frame
// viewport sizes are separate; no framebuffer mapping crosses this interface.
inline int screenWidth = 0, screenHeight = 0, screenAvailableHeight = 0;
inline int windowX = 0, windowY = 0, windowWidth = 0, windowHeight = 0;

inline bool setScreenSize(int width, int height, int availableHeight) {
    if (width < 1 || height < 1 || width > 32768 || height > 32768 || availableHeight < 1 ||
        availableHeight > height)
        return false;
    screenWidth = width;
    screenHeight = height;
    screenAvailableHeight = availableHeight;
    return true;
}

inline bool setWindowGeometry(int x, int y, int width, int height) {
    if (x < -32768 || x > 32768 || y < -32768 || y > 32768 || width < 1 || height < 1 ||
        width > 32768 || height > 32768)
        return false;
    windowX = x;
    windowY = y;
    windowWidth = width;
    windowHeight = height;
    return true;
}
}
