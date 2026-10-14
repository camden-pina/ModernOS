include(platform/Adwaita.cmake)
include(platform/GCrypt.cmake)
include(platform/ImageDecoders.cmake)
include(platform/Skia.cmake)
include(platform/Soup.cmake)

list(APPEND WebCore_UNIFIED_SOURCE_LIST_FILES "SourcesModernOS.txt" "platform/SourcesGLib.txt")
list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
    "${WEBCORE_DIR}/platform/glib"
    "${WEBCORE_DIR}/platform/graphics/glib"
    "${WEBCORE_DIR}/platform/graphics/egl"
    "${WEBCORE_DIR}/platform/graphics/opentype"
    "${WEBCORE_DIR}/platform/video-codecs"
    "${WEBCORE_DIR}/platform/network/glib"
    "${WEBCORE_DIR}/platform/text/icu"
    "${WEBCORE_DIR}/platform/wpe"
)
list(APPEND WebCore_LIBRARIES GLib::Module Tasn1::Tasn1)
list(APPEND WebCore_SOURCES "${MODERNOS_PORT_DIR}/GraphicsLayerModernOS.cpp")
