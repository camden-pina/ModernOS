// Real generic-family aliases for the fonts shipped in this OS. Fontconfig
// receives no host filesystem paths or writable disk-cache requirements.
#pragma once

#include <fontconfig/fontconfig.h>

static bool modernOSFonts() {
    static constexpr char aliases[] = R"XML(<?xml version="1.0"?>
<fontconfig>
<alias><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias>
<alias><family>serif</family><prefer><family>DejaVu Serif</family></prefer></alias>
<alias><family>monospace</family><prefer><family>DejaVu Sans Mono</family></prefer></alias>
<alias><family>system-ui</family><prefer><family>DejaVu Sans</family></prefer></alias>
</fontconfig>)XML";
    FcConfig *config = FcConfigCreate();
    if (!config)
        return false;
    bool ok =
        FcConfigSetRescanInterval(config, 0) &&
        FcConfigParseAndLoadFromMemory(config, reinterpret_cast<const FcChar8 *>(aliases), FcTrue);
    for (const char *name :
         {"DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSans-Oblique.ttf",
          "DejaVuSans-BoldOblique.ttf", "DejaVuSerif.ttf", "DejaVuSerif-Bold.ttf",
          "DejaVuSansMono.ttf", "DejaVuSansMono-Bold.ttf"}) {
        char path[128];
        snprintf(path, sizeof(path), "/usr/share/fonts/TTF/%s", name);
        ok = ok && FcConfigAppFontAddFile(config, reinterpret_cast<const FcChar8 *>(path));
    }

    ok = ok && FcConfigSetCurrent(config);
    FcConfigDestroy(config);
    return ok;
}
