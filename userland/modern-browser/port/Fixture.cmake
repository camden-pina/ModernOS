# Native software browser and its controlled rendering/network check modes.
find_package(GLibCompileResources REQUIRED)
GLIB_COMPILE_RESOURCES(
    OUTPUT "${WebCore_DERIVED_SOURCES_DIR}/BrowserResources.c"
    SOURCE_XML "${MODERNOS_PORT_DIR}/BrowserResources.xml"
    RESOURCE_DIRS "${WEBCORE_DIR}/Resources" "${CMAKE_SOURCE_DIR}/Source/WebKit/Resources/wpe")
include("${CMAKE_SOURCE_DIR}/Source/WebKit/ModernMediaControlsGResources.cmake")
WEBKIT_BUILD_MODERN_MEDIA_CONTROLS_GRESOURCES("${WebCore_DERIVED_SOURCES_DIR}")
add_executable(ModernOSRenderer "${MODERNOS_PORT_DIR}/renderer-fixture.cpp"
    "${MODERNOS_PORT_DIR}/MediaModernOS.cpp"
    "${WebCore_DERIVED_SOURCES_DIR}/BrowserResources.c"
    "${WebCore_DERIVED_SOURCES_DIR}/ModernMediaControlsGResourceBundle.c")
get_target_property(modernos_webcore_includes WebCore INCLUDE_DIRECTORIES)
target_include_directories(ModernOSRenderer PRIVATE "${CMAKE_BINARY_DIR}"
    ${modernos_webcore_includes} "${MODERNOS_PORT_DIR}")
# The optional exception observer needs DebuggerScope's internal lexical-slot
# declarations. Keep these search paths local to the embedding translation
# unit, preserving the native media backend's original header environment.
set_property(SOURCE "${MODERNOS_PORT_DIR}/renderer-fixture.cpp" APPEND PROPERTY
    INCLUDE_DIRECTORIES "${WEBCORE_DIR}" "${JAVASCRIPTCORE_DIR}/debugger"
    "${CMAKE_BINARY_DIR}/JavaScriptCore/PrivateHeaders/JavaScriptCore")
target_compile_definitions(ModernOSRenderer PRIVATE BUILDING_WebCore BUILDING_WEBKIT=1 BUILDING_WITH_CMAKE=1)
execute_process(COMMAND pkg-config --static --libs
    gio-unix-2.0 gthread-2.0 libsoup-3.0 icu-i18n harfbuzz harfbuzz-icu
    fontconfig freetype2 libgcrypt libxml-2.0 libwebpdemux libwebpmux libjpeg
    libpng zlib sqlite3 libtasn1 libavformat libavcodec libavutil libswscale libswresample
    OUTPUT_VARIABLE modernos_fixture_libs OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
separate_arguments(modernos_fixture_libs UNIX_COMMAND "${modernos_fixture_libs}")
target_link_libraries(ModernOSRenderer PRIVATE -Wl,--start-group WebCore PAL JavaScriptCore WTF Skia
    ${modernos_fixture_libs} -Wl,--end-group)
target_link_options(ModernOSRenderer PRIVATE -static -no-pie -Wl,--gc-sections)
set_target_properties(ModernOSRenderer PROPERTIES OUTPUT_NAME web-renderer
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
