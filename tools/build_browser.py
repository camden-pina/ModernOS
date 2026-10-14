#!/usr/bin/env python3
"""Build the genuine NetSurf engine with ModernOS's offscreen pipe surface.

Only generated build copies are patched; the pinned upstream bundle stays
unchanged. This backend owns browser content, never the desktop compositor.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile

from build_web_deps import main as build_dependencies, copy_stage, stage_lock, FLAGS, MUSL, PREFIX

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/browser'
SOURCE = BUILD / 'netsurf-all-3.11'
VENDOR = ROOT / 'userland/vendor'
ARCHIVE = VENDOR / 'netsurf-all-3.11.tar.gz'
HASH = '4dea880ff3c2f698bfd62c982b259340f9abcd7f67e6c8eb2b32c61f71644b7b'
GPERF_HASH = 'fd87e0aba7e43ae054837afd6cd4db03a3f2693deb3619085e6ed9d8d9604ad8'
LIBRARIES = ['libnslog', 'libwapcaplet', 'libparserutils', 'libhubbub', 'libdom',
             'libcss', 'libnsbmp', 'libnsgif', 'libnsutils', 'libutf8proc',
             'libnspsl', 'libsvgtiny', 'libnsfb']


def run(where, name, args, env):
    log = BUILD / (name + '.log')
    with log.open('wb') as output:
        result = subprocess.run(args, cwd=where, env=env, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f'{name} failed; see {log}')


def replace(path, before, after):
    data = path.read_text()
    if before not in data:
        raise SystemExit('Upstream browser patch context missing: ' + str(path))
    path.write_text(data.replace(before, after, 1))


def build():
    global SOURCE
    if hashlib.sha256(ARCHIVE.read_bytes()).hexdigest() != HASH:
        raise SystemExit('NetSurf upstream source checksum mismatch')

    owned = [ROOT / 'userland/browser/surface.c', ROOT / 'userland/browser/actions.inc',
             ROOT / 'userland/include/browser_protocol.h', Path(__file__),
             ROOT / 'userland/browser/downloads.inc', ROOT / 'userland/browser/path.inc',
             ROOT / 'userland/browser/stacking.inc', ROOT / 'userland/browser/fetch_policy.inc',
             ROOT / 'userland/browser/box_order.inc', ROOT / 'userland/browser/hit_test.inc',
             ROOT / 'userland/browser/performance.inc']

    digest = hashlib.sha256(HASH.encode())
    for path in owned:
        digest.update(path.read_bytes())
    digest.update(FLAGS.encode())
    for name in ('curl', 'ssl', 'crypto', 'z', 'png16', 'jpeg', 'freetype', 'expat'):
        digest.update((PREFIX / 'lib' / ('lib' + name + '.a')).read_bytes())
    key = digest.hexdigest()

    BUILD.mkdir(parents=True, exist_ok=True)

    # Keep each prepared source/build cache. Changed port inputs get a fresh
    # pinned extraction instead of deleting the previous compiled revision.
    SOURCE = BUILD / 'sources' / key / 'netsurf-all-3.11'
    SOURCE.parent.mkdir(parents=True, exist_ok=True)

    gperf_archive = VENDOR / 'gperf-3.3.tar.gz'
    if hashlib.sha256(gperf_archive.read_bytes()).hexdigest() != GPERF_HASH:
        raise SystemExit('GNU gperf upstream source checksum mismatch')
    host_tools = BUILD / 'host-tools'
    if not (host_tools / 'bin/gperf').exists():
        with tarfile.open(gperf_archive) as contents:
            contents.extractall(BUILD, filter='data')
        gperf = BUILD / 'gperf-3.3'
        host_env = os.environ.copy()
        run(gperf, 'gperf-configure', ['./configure', '--prefix=' + str(host_tools)], host_env)
        run(gperf, 'gperf-build', ['make', '-j8'], host_env)
        run(gperf, 'gperf-install', ['make', 'install'], host_env)

    stamp = SOURCE.parent / '.prepared'
    if not stamp.exists() or stamp.read_text() != key:
        if SOURCE.exists():
            raise SystemExit('Incomplete NetSurf preparation; preserve and inspect ' + str(SOURCE))
        with tarfile.open(ARCHIVE) as contents:
            contents.extractall(SOURCE.parent, filter='data')
        nsfb = SOURCE / 'libnsfb'
        shutil.copy2(owned[0], nsfb / 'src/surface/modernos.c')
        shutil.copy2(owned[2], nsfb / 'include/browser_protocol.h')
        replace(nsfb / 'include/libnsfb.h', '    NSFB_SURFACE_COUNT,',
                '    NSFB_SURFACE_MODERNOS, /**< ModernOS offscreen browser */\n    NSFB_SURFACE_COUNT,')
        replace(nsfb / 'src/surface/Makefile', 'surface.c ram.c', 'surface.c ram.c modernos.c')

        # musl provides iconv in libc; NetSurf's non-glibc defaults otherwise
        # assume a separate GNU libiconv that is neither needed nor installed.
        replace(SOURCE / 'netsurf/Makefile', ',-ULIBICONV_PLUG,-liconv))', ',-ULIBICONV_PLUG,))')

        # File fetches need only a read snapshot. Use the kernel's real private
        # file mapping; coherent MAP_SHARED file mappings are not implemented.
        replace(SOURCE / 'netsurf/content/fetchers/file/file.c',
                'mmap(NULL, buf_size, PROT_READ, MAP_SHARED, fd, 0)',
                'mmap(NULL, buf_size, PROT_READ, MAP_PRIVATE, fd, 0)')
        replace(SOURCE / 'netsurf/frontends/framebuffer/fetch.c',
                '\tl = strlen(unix_path);',
                '\tconst char *extension = strrchr(unix_path, \'.\');\n'
                '\tif (extension && (strcasecmp(extension, ".js") == 0 || strcasecmp(extension, ".mjs") == 0))\n'
                '\t\treturn "text/javascript";\n'
                '\tif (extension && strcasecmp(extension, ".txt") == 0) return "text/plain";\n'
                '\tl = strlen(unix_path);')

        # OpenSSL treats set1_host("10.0.2.2") as a DNS SAN, unlike curl's
        # built-in verifier. Preserve certificate-chain validation while using
        # the IP SAN verifier for actual numeric addresses.
        fetch = SOURCE / 'netsurf/content/fetchers/curl.c'
        replace(fetch, '#include <errno.h>', '#include <errno.h>\n#include <arpa/inet.h>')
        replace(fetch, '\tok = X509_VERIFY_PARAM_set1_host(vparam,\n\t\t\t\t\t lwc_string_data(f->host),\n\t\t\t\t\t lwc_string_length(f->host));',
                '\tunsigned char address[16];\n\tconst char *hostname = lwc_string_data(f->host);\n'
                '\tif (inet_pton(AF_INET, hostname, address) == 1 || inet_pton(AF_INET6, hostname, address) == 1)\n'
                '\t\tok = X509_VERIFY_PARAM_set1_ip_asc(vparam, hostname);\n'
                '\telse\n\t\tok = X509_VERIFY_PARAM_set1_host(vparam, hostname, lwc_string_length(f->host));')

        # Web documents may not fetch local files, including directories the
        # desktop previously granted for a user-selected local document.
        replace(SOURCE / 'netsurf/content/fetch.c', '#include "netsurf/misc.h"',
                '#include "netsurf/misc.h"\n#include "netsurf/fetch.h"')
        replace(SOURCE / 'netsurf/content/fetch.c', 'nserror\nfetch_start(',
                owned[7].read_text() + '\nnserror\nfetch_start(')
        replace(SOURCE / 'netsurf/content/fetch.c',
                '\t/* try and obtain a fetcher for this scheme */',
                '\tif (!fetch_can_access(url, referer)) {\n'
                '\t\tlwc_string_unref(scheme); free(fetch); return NSERROR_PERMISSION;\n'
                '\t}\n\n\t/* try and obtain a fetcher for this scheme */')
        replace(SOURCE / 'netsurf/content/fetch.h', 'bool fetch_can_fetch(const nsurl *url);',
                'bool fetch_can_fetch(const nsurl *url);\n'
                'bool fetch_can_access(const nsurl *url, const nsurl *referer);')

        # This function handles both cache hits and redirect targets. Checking
        # only fetch_start would let a remote page reuse cached local content.
        replace(SOURCE / 'netsurf/content/llcache.c',
                '\t/* Get rid of any url fragment */',
                '\tif (!fetch_can_access(url, referer)) return NSERROR_PERMISSION;\n\n'
                '\t/* Get rid of any url fragment */')

        # Upstream's target-scheme test accidentally inspects the source three
        # times. A top-level HTTP redirect has no referrer, so reject its local
        # destination here before it can look up a cached file.
        cache = SOURCE / 'netsurf/content/llcache.c'
        cache_data = cache.read_text()
        start = cache_data.index(
            '\t\t/* file, about and resource are not valid redirect targets */')
        end = cache_data.index('\n\tlwc_string_unref(scheme);', start)
        cache.write_text(cache_data[:start] + cache_data[start:end].replace(
            'lwc_string_isequal(object_scheme,', 'lwc_string_isequal(scheme,').replace(
            'return NSERROR_OK;', 'return NSERROR_PERMISSION;') + cache_data[end:])

        window_binding = SOURCE / 'netsurf/content/handlers/javascript/duktape/Window.bnd'
        replace(window_binding, '#define HANDLER_MAGIC MAGIC(HANDLER_MAP)',
                '#define HANDLER_MAGIC MAGIC(HANDLER_MAP)\n#include <time.h>\n' + owned[10].read_text())
        replace(window_binding, '\tduk_put_prop_string(ctx, 0, WINDOW_CALLBACKS);',
                '\tduk_put_prop_string(ctx, 0, WINDOW_CALLBACKS);\n\tmodernos_performance_install(ctx, 0);')

        plot = SOURCE / 'netsurf/frontends/framebuffer/framebuffer.c'
        replace(plot, '#include <stdio.h>', '#include <stdio.h>\n#include <math.h>\n#include <stdint.h>')
        replace(plot, 'static nserror\nframebuffer_plot_path(',
                owned[5].read_text() + '\nstatic nserror\nframebuffer_plot_path(')
        replace(plot, '\tNSLOG(netsurf, INFO, "path unimplemented");\n\treturn NSERROR_OK;',
                '\treturn modernos_plot_path(ctx, pstyle, p, n, transform);')

        # Rejected glyphs must not stall hit testing or line breaking.
        font = SOURCE / 'netsurf/frontends/framebuffer/font_freetype.c'
        font_data = font.read_text()
        font_start = font_data.index('fb_font_position(')
        font.write_text(font_data[:font_start] + font_data[font_start:].replace(
            'if (glyph == NULL)\n                        continue;',
            'if (glyph == NULL) {\n                        nxtchr = utf8_next(string, length, nxtchr);\n                        continue;\n                }'))

        redraw = SOURCE / 'netsurf/content/handlers/html/redraw.c'
        redraw_data = redraw.read_text()
        child_start = redraw_data.index('static bool html_redraw_box_children(')
        child_end = redraw_data.index('\n/**', child_start)
        redraw_data = redraw_data[:child_start] + owned[8].read_text() + '\n' + owned[6].read_text() + '\n' + redraw_data[child_end:]
        redraw.write_text(redraw_data)
        replace(redraw, '#include "desktop/gui_internal.h"',
                '#include "desktop/gui_internal.h"\n#include "desktop/browser_private.h"\n#include "netsurf/window.h"')
        replace(redraw, '\t/* avoid trivial FP maths */', '''
    if (box->style && css_computed_position(box->style) == CSS_POSITION_FIXED && html->bw) {
        bool fixed_parent = false;
        for (struct box *p = box->parent; p; p = p->parent)
            if (p->style && css_computed_position(p->style) == CSS_POSITION_FIXED) fixed_parent = true;
        if (!fixed_parent) {
            int sx, sy; modernos_fixed_offset(html, box, &sx, &sy);
            x_parent += sx; y_parent += sy;
        }
    }
\t/* avoid trivial FP maths */''')

        inspect = SOURCE / 'netsurf/content/handlers/html/box_inspect.c'
        inspect_data = inspect.read_text()
        hit_start = inspect_data.index('struct box *\nbox_at_point(')
        hit_end = inspect_data.index('\n\n/* Exported function', hit_start)
        inspect_data = inspect_data[:hit_start] + owned[8].read_text() + '\n' + owned[9].read_text() + inspect_data[hit_end:]
        inspect.write_text(inspect_data)
        replace(inspect, '#include "desktop/scrollbar.h"',
                '#include "desktop/scrollbar.h"\n#include "netsurf/browser_window.h"\n#include "desktop/browser_private.h"\n'
                '#include "desktop/gui_internal.h"\n#include "netsurf/window.h"')

        inspect_header = SOURCE / 'netsurf/content/handlers/html/box_inspect.h'
        replace(inspect_header, 'box_at_point(const css_unit_ctx *unit_len_ctx,',
                'box_at_point(const struct html_content *html,')

        for caller in ('interaction.c', 'html.c'):
            path = inspect.parent / caller
            path.write_text(path.read_text().replace(
                'box_at_point(&html->unit_len_ctx,', 'box_at_point(html,'))

        replace(inspect, '\tbox = html->layout;\n\tbx = box->margin[LEFT];', '''
    unsigned budget = 65536;
    struct box *hit = modernos_pick_box(html, html->layout, x, y, 0, &budget);
    if (hit && hit->text && !hit->object) {
        modernos_box_position(html, hit, &tx, &ty);
        *dx = x - tx; *dy = y - ty;
        return hit;
    }
\tbox = html->layout;
\tbx = box->margin[LEFT];''')
        layout = SOURCE / 'netsurf/content/handlers/html/layout.c'
        replace(layout, '\t\t\tif (!layout_absolute(c, containing_block,\n\t\t\t\t\tcx, cy, content))\n\t\t\t\treturn false;', '''
            if (css_computed_position(c->style) == CSS_POSITION_FIXED) {
                struct box viewport = *content->layout;
                viewport.type = BOX_BLOCK;
                viewport.width = FIXTOINT(css_unit_len2device_px(c->style, &content->unit_len_ctx,
                    content->unit_len_ctx.viewport_width, CSS_UNIT_PX));
                viewport.height = FIXTOINT(css_unit_len2device_px(c->style, &content->unit_len_ctx,
                    content->unit_len_ctx.viewport_height, CSS_UNIT_PX));
                for (unsigned side = 0; side < 4; side++) viewport.padding[side] = 0;
                int px, py; box_coords(box, &px, &py);
                if (!layout_absolute(c, &viewport, px, py, content)) return false;
            } else if (!layout_absolute(c, containing_block, cx, cy, content)) return false;''')

        # Viewport dimensions are already CSS lengths. Convert the complete
        # length once; len2device_px rounds each CSS-pixel unit at unusual DPI.
        layout.write_text(layout.read_text().replace(
            'css_unit_len2device_px(c->style, &content->unit_len_ctx,\n'
            '                    content->unit_len_ctx.viewport_width, CSS_UNIT_PX)',
            'css_unit_css2device_px(content->unit_len_ctx.viewport_width, content->unit_len_ctx.device_dpi)').replace(
            'css_unit_len2device_px(c->style, &content->unit_len_ctx,\n'
            '                    content->unit_len_ctx.viewport_height, CSS_UNIT_PX)',
            'css_unit_css2device_px(content->unit_len_ctx.viewport_height, content->unit_len_ctx.device_dpi)'))
        replace(SOURCE / 'netsurf/content/handlers/html/box.h',
                '\tIS_REPLACED = 1 << 12',
                '\tHAS_FIXED_DESCENDANTS = 1 << 13, /* viewport-fixed paint bounds */\n\tIS_REPLACED = 1 << 12')
        layout_data = layout.read_text()
        bounds_start = layout_data.index('static void layout_calculate_descendant_bboxes(')
        bounds_end = layout_data.index(
            '/* exported function documented in html/layout.h */', bounds_start)
        bounds = layout_data[bounds_start:bounds_end]
        bounds = bounds.replace('\tstruct box *child;',
                                '\tstruct box *child;\n\tbox->flags &= ~HAS_FIXED_DESCENDANTS;\n'
                                '\tif (box->style && css_computed_position(box->style) == CSS_POSITION_FIXED)\n'
                                '\t\tbox->flags |= HAS_FIXED_DESCENDANTS;', 1)
        bounds = bounds.replace('layout_calculate_descendant_bboxes(unit_len_ctx, child);',
                                'layout_calculate_descendant_bboxes(unit_len_ctx, child);\n'
                                '\t\tif (child->flags & HAS_FIXED_DESCENDANTS) box->flags |= HAS_FIXED_DESCENDANTS;')
        layout.write_text(layout_data[:bounds_start] + bounds + layout_data[bounds_end:])
        replace(redraw, '\t/* return if the rectangle is completely outside the clip rectangle */', '''
    /* Fixed descendants can remain visible while their parent scrolls out of
     * view. Keep visible overflow open through each ancestor's paint bounds. */
    if (box->flags & HAS_FIXED_DESCENDANTS) {
        if (overflow_x == CSS_OVERFLOW_VISIBLE || !box->parent) {
            r.x0 = min(r.x0, clip->x0); r.x1 = max(r.x1, clip->x1);
        }
        if (overflow_y == CSS_OVERFLOW_VISIBLE || !box->parent) {
            r.y0 = min(r.y0, clip->y0); r.y1 = max(r.y1, clip->y1);
        }
    }
\t/* return if the rectangle is completely outside the clip rectangle */''')
        gui = SOURCE / 'netsurf/frontends/framebuffer/gui.c'
        replace(gui, '#include <stdint.h>', '#define _GNU_SOURCE 1\n#include <stdint.h>\n#include "browser_protocol.h"\n'
                '#include <errno.h>\n#include <stdio.h>\n#include <fcntl.h>\n#include <unistd.h>\n'
                '#include <sys/stat.h>\n#include <sys/syscall.h>\n'
                '#include "desktop/download.h"\n#include "netsurf/download.h"\n'
                'void modernos_browser_text(unsigned type, const char *value);\n'
                'void modernos_browser_state(unsigned flags);\n'
                'void modernos_browser_action(unsigned action, const char *text);\n'
                'static void modernos_download_cancel_all(void);')
        replace(gui, '#include "netsurf/browser_window.h"',
                '#include "netsurf/browser_window.h"\n#include "netsurf/browser.h"')
        replace(gui, '\t/* common initialisation */',
                '\t/* Match CSS reference pixels and FreeType metrics on the desktop surface. */\n'
                '\t(void)browser_set_dpi(96);\n\n\t/* common initialisation */')
        shutil.copy2(owned[2], gui.parent / 'browser_protocol.h')
        replace(gui, '\tNSLOG(netsurf, INFO, "%s %s", warning, detail);',
                '\tNSLOG(netsurf, INFO, "%s %s", warning, detail);\n'
                '\tmodernos_browser_text(BROWSER_NOTICE, detail ? detail : messages_get(warning));')
        replace(gui, '\tfbtk_set_text(g->url, nsurl_access(url));',
                '\tfbtk_set_text(g->url, nsurl_access(url));\n'
                '\tmodernos_browser_text(BROWSER_URL, nsurl_access(url));')

        # Reuse the upstream content widgets and navigation core, but expose
        # navigation in the native shared toolbar. Hidden widgets remain alive
        # for the core callbacks; they cannot draw or receive pointer input.
        replace(gui, '\tif (toolbar != NULL) {\n\t\ttoolbar_height = fbtk_get_height(toolbar);\n\t} else {\n\t\ttoolbar_height = 0;\n\t}',
                '\tif (toolbar != NULL) fbtk_set_mapping(toolbar, false);\n\ttoolbar_height = 0;')
        replace(gui, '\tint toolbar_height = fbtk_get_height(gw->toolbar);', '\tint toolbar_height = 0;')

        # Clamp short documents to zero. Repaint on scroll so fixed elements
        # and backgrounds stay in place instead of moving copied pixels.
        gui_data = gui.read_text().replace('(content_height - height)', 'max(0, content_height - height)')
        gui_data = gui_data.replace('(content_width - width)', 'max(0, content_width - width)')
        pan_start = gui_data.index('static void\nfb_pan(')
        pan_end = gui_data.index('\nstatic void\nfb_redraw(', pan_start)
        gui_data = gui_data[:pan_start] + '''static void
fb_pan(fbtk_widget_t *widget, struct browser_widget_s *bwidget,
       struct browser_window *bw)
{
    (void)bw;
    bwidget->scrolly += bwidget->pany;
    bwidget->scrollx += bwidget->panx;
    bwidget->pan_required = false;
    bwidget->panx = bwidget->pany = 0;
    fb_queue_redraw(widget, 0, 0, fbtk_get_width(widget), fbtk_get_height(widget));
}
''' + gui_data[pan_end:]
        gui.write_text(gui_data)
        replace(gui, '\tresize_toolbar(gw, toolbar_height, 2,',
                '\tresize_toolbar(gw, fbtk_get_height(gw->toolbar), 2,')
        replace(gui, '\t\t\t&right_arrow : &right_arrow_g);\n}',
                '\t\t\t&right_arrow : &right_arrow_g);\n'
                '\tmodernos_browser_state((browser_window_back_available(bw) ? BROWSER_CAN_BACK : 0) |\n'
                '\t\t(browser_window_forward_available(bw) ? BROWSER_CAN_FORWARD : 0) |\n'
                '\t\t(gw->throbber_index >= 0 ? BROWSER_LOADING : 0));\n}')
        replace(gui, 'gui_window_start_throbber(struct gui_window *g)\n{\n\tg->throbber_index = 0;\n\tframebuffer_schedule(100, throbber_advance, g);\n}',
                'gui_window_start_throbber(struct gui_window *g)\n{\n\tg->throbber_index = 0;\n\tfb_update_back_forward(g);\n}')
        replace(gui, 'static struct gui_window_table framebuffer_window_table = {',
                'static void modernos_set_title(struct gui_window *g, const char *title)\n'
                '{ (void)g; modernos_browser_text(BROWSER_TITLE, title ? title : "Browser"); }\n\n'
                'static struct gui_window_table framebuffer_window_table = {\n'
                '\t.set_title = modernos_set_title,')
        replace(gui, 'static struct gui_misc_table framebuffer_misc_table = {',
                owned[4].read_text() + '\nstatic struct gui_misc_table framebuffer_misc_table = {')
        replace(gui, '\t\t.misc = &framebuffer_misc_table,',
                '\t\t.misc = &framebuffer_misc_table,\n\t\t.download = &modernos_download_table,')
        replace(gui, '\turldb_save_cookies(nsoption_charp(cookie_jar));',
                '\tmodernos_download_cancel_all();\n\turldb_save_cookies(nsoption_charp(cookie_jar));')
        replace(gui, '\tnsoption_setnull_charp(cookie_file, strdup("~/.netsurf/Cookies"));',
                '\tchar cookie_path[512], config_path[512];\n'
                '\tconst char *home = getenv("HOME");\n'
                '\tif (!home || home[0] != \'/\') home = "/home/user";\n'
                '\tif (snprintf(config_path, sizeof(config_path), "%s/.config", home) >= (int)sizeof(config_path)) return NSERROR_BAD_PARAMETER;\n'
                '\t(void)mkdir(config_path, 0755);\n'
                '\tif (snprintf(config_path, sizeof(config_path), "%s/.config/netsurf", home) >= (int)sizeof(config_path)) return NSERROR_BAD_PARAMETER;\n'
                '\t(void)mkdir(config_path, 0755);\n'
                '\tif (snprintf(cookie_path, sizeof(cookie_path), "%s/Cookies", config_path) >= (int)sizeof(cookie_path)) return NSERROR_BAD_PARAMETER;\n'
                '\tnsoption_setnull_charp(cookie_file, strdup(cookie_path));')
        replace(gui, '\tnsoption_setnull_charp(cookie_jar, strdup("~/.netsurf/Cookies"));',
                '\tnsoption_setnull_charp(cookie_jar, strdup(cookie_path));')
        event = SOURCE / 'netsurf/frontends/framebuffer/fbtk/event.c'
        replace(event, '#include <stdint.h>', '#include <stdint.h>\n#include "../browser_protocol.h"')
        replace(event, '\tint ucs4 = -1;',
                '\tint ucs4 = -1;\n'
                '\tif (code >= BROWSER_TEXT_KEY_BASE + 32 && code <= BROWSER_TEXT_KEY_BASE + 126) return code - BROWSER_TEXT_KEY_BASE;')

        # The upstream data-failure path aborts the fetch without notifying the
        # frontend. Release its context and remove the partial file as well.
        replace(SOURCE / 'netsurf/desktop/download.c',
                '\t\t\tif (error != NSERROR_OK)\n\t\t\t\tllcache_handle_abort(handle);',
                '\t\t\tif (error != NSERROR_OK) {\n'
                '\t\t\t\tllcache_handle_abort(handle);\n'
                '\t\t\t\tguit->download->error(ctx->window, "Unable to write the download; the partial file was removed.");\n'
                '\t\t\t}')
        with gui.open('a') as output:
            output.write('\n' + owned[1].read_text())
        stamp.write_text(key)

    env = os.environ.copy()
    env.update(CFLAGS=FLAGS, LDFLAGS='-static -no-pie -L' + str(PREFIX / 'lib'),
               PKG_CONFIG_LIBDIR=str(PREFIX / 'lib/pkgconfig'), PKG_CONFIG_PATH='')
    env['PATH'] = str(host_tools / 'bin') + ':' + str(PREFIX / 'bin') + ':' + env['PATH']
    host = subprocess.check_output(['gcc', '-dumpmachine'], text=True).strip()
    common = ['make', '-j8', 'PREFIX=' + str(PREFIX),
              'NSSHARED=' + str(SOURCE / 'buildsystem'), 'BUILD=' + host,
              'HOST=x86_64-linux-musl', 'CC=' + str(MUSL), 'AR=ar', 'BUILD_CC=gcc',
              'PKGCONFIG=pkg-config --static', 'WARNFLAGS=-Wall -Wextra -Wno-error',
              'COMPONENT_TYPE=lib-static']
    for library in LIBRARIES:
        options = []
        if library == 'libnsfb':
            options = [name + '=no' for name in ['NSFB_SDL_AVAILABLE', 'NSFB_XCB_AVAILABLE',
                                                 'NSFB_WLD_AVAILABLE', 'NSFB_VNC_AVAILABLE']]
        local_env = env.copy()
        if library == 'libparserutils':
            local_env['CFLAGS'] += ' -DWITH_ICONV_FILTER'
        run(SOURCE / library, library + '-build', common + options, local_env)
        run(SOURCE / library, library + '-install', common + options + ['install'], local_env)

    # IDL binding generator executes on the build host, not in ModernOS.
    gen = ['make', '-j8', 'PREFIX=' + str(PREFIX), 'NSSHARED=' + str(SOURCE / 'buildsystem'),
           'CC=gcc', 'HOST=' + host, 'BUILD=' + host]
    host_env = os.environ.copy()
    run(SOURCE / 'nsgenbind', 'nsgenbind-build', gen, host_env)
    run(SOURCE / 'nsgenbind', 'nsgenbind-install', gen + ['install'], host_env)
    options = ['TARGET=framebuffer', 'NETSURF_FB_FRONTEND=modernos',
               'NETSURF_FB_FONTLIB=freetype', 'NETSURF_USE_LIBICONV_PLUG=NO',
               'NETSURF_FB_FONT_CURSIVE=DejaVuSans.ttf', 'NETSURF_FB_FONT_FANTASY=DejaVuSerif.ttf',
               'NETSURF_FB_FONTPATH=/usr/share/fonts/TTF',
               'NETSURF_FB_RESPATH=$${NETSURFRES}:/usr/share/netsurf',
               'NETSURF_FRAMEBUFFER_RESOURCES=' + str(PREFIX / 'share/netsurf')]
    options += ['NETSURF_USE_' + feature + '=YES' for feature in
                ['CURL', 'OPENSSL', 'PNG', 'JPEG', 'GIF', 'BMP', 'NSSVG', 'DUKTAPE',
                 'UTF8PROC', 'NSPSL', 'NSLOG']]
    options += ['NETSURF_USE_' + feature + '=NO' for feature in
                ['ROSPRITE', 'WEBP', 'JPEGXL', 'HARU_PDF', 'VIDEO']]
    run(SOURCE / 'netsurf', 'netsurf-build', common + options, env)
    run(SOURCE / 'netsurf', 'netsurf-install', common + options + ['install'], env)
    runtime = ROOT / 'userland/runtime'
    shutil.copy2(PREFIX / 'bin/netsurf-fb', ROOT / 'userland/bin/netsurf')
    shutil.copytree(PREFIX / 'share/netsurf', runtime / 'usr/share/netsurf', dirs_exist_ok=True)
    shutil.copy2(ROOT / 'userland/browser/modernos.html',
                 runtime / 'usr/share/netsurf/modernos.html')
    shutil.copy2(ROOT / 'userland/browser/Choices', runtime / 'usr/share/netsurf/Choices')
    for directory in ('include', 'lib'):
        copy_stage(PREFIX / directory, runtime / 'usr' / directory)
    fonts = runtime / 'usr/share/fonts/TTF'
    fonts.mkdir(parents=True, exist_ok=True)
    for name in ('DejaVuSans', 'DejaVuSans-Bold', 'DejaVuSans-Oblique', 'DejaVuSans-BoldOblique',
                 'DejaVuSerif', 'DejaVuSerif-Bold', 'DejaVuSansMono', 'DejaVuSansMono-Bold'):
        shutil.copy2(ROOT / 'userland/browser/fonts' / (name + '.ttf'), fonts)
    shutil.copy2(ROOT / 'userland/browser/fonts/LICENSE.txt',
                 VENDOR / 'licenses/dejavu-LICENSE.txt')

    for library in LIBRARIES + ['netsurf', 'nsgenbind']:
        for name in ('COPYING', 'LICENSE', 'COPYING-MIT', 'COPYING-ISC'):
            license_file = SOURCE / library / name
            if license_file.is_file():
                shutil.copy2(license_file, VENDOR / 'licenses' / (library + '-' + name + '.txt'))

    # Make the installed SDK metadata usable by the guest's /usr prefix.
    for pc in (runtime / 'usr/lib/pkgconfig').glob('*.pc'):
        pc.write_text(pc.read_text().replace(str(PREFIX), '/usr'))

    print('Built NetSurf 3.11: real HTML/CSS/DOM, images, FreeType and Duktape, ModernOS pipe surface.')


def main():
    build_dependencies()
    with stage_lock():
        build()


if __name__ == '__main__':
    main()
