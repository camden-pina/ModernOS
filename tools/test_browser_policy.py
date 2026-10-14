#!/usr/bin/env python3
"""Remote pages cannot embed or navigate to cached, previously opened files."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
from browser_protocol import Browser, ROOT


def main():
    work = ROOT / 'build/browser-policy-test'
    work.mkdir(parents=True, exist_ok=True)
    image = work / 'private.svg'
    image.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="200" height="80">'
                     '<path fill="red" d="M0 0H200V80H0Z"/></svg>')
    script = work / 'private.js'
    script.write_text(
        'document.write(\'<div style="position:absolute;left:300px;top:0;width:20px;height:20px;background:red"></div>\');')
    local = work / 'local.html'
    local.write_text('<!doctype html><title>Private local document</title>'
                     '<body style="margin:0;background:white"><script src="private.js"></script><img src="private.svg"></body>')

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            if self.path == '/redirect-local':
                self.send_response(302)
                self.send_header('Location', local.as_uri())
                self.send_header('Content-Length', '0')
                self.end_headers()
                return

            base = '<base href="' + work.as_uri() + '/">' if self.path == '/base.html' else ''
            body = ('<!doctype html><title>Remote file access test</title>' + base +
                    '<body style="margin:0;background:white">'
                    '<script src="' + script.as_uri() + '"></script>'
                    '<img width="200" height="80" src="' + image.as_uri() + '">'
                    '<img width="200" height="80" src="private.svg">'
                    '<iframe width="200" height="80" src="' + local.as_uri() + '"></iframe>'
                    '<p><a style="display:block;width:300px;height:60px" href="' + local.as_uri() +
                    '">Try opening a local file</a></p></body>').encode()

            if self.path == '/link.html':
                body = ('<!doctype html><title>Remote link test</title><body style="margin:0;background:white">'
                        '<a href="' + local.as_uri() + '" style="display:block;width:300px;height:60px;background:#00ff00">'
                        'Try opening a local file</a></body>').encode()

            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    browser = Browser(work, local.as_uri())
    try:
        browser.ready('Private local document')
        assert browser.rgb(20, 20) == (255, 0, 0), 'Local fixture was not actually loaded'
        assert browser.rgb(310, 10) == (255, 0, 0), 'Local JavaScript fixture was not executed'
        url = f'http://127.0.0.1:{server.server_port}'
        for name in ('page.html', 'base.html'):
            browser.navigate(url + '/' + name, 'Remote file access test')
            browser.screenshot('denied-' + name)
            red = sum(browser.pixels[i:i + 3] ==
                      b'\x00\x00\xff' for i in range(0, len(browser.pixels), 4))
            assert red == 0, f'Remote document obtained a cached local image through {name}'

        browser.navigate(url + '/link.html', 'Remote link test')

        assert browser.rgb(150, 40) == (0, 255, 0), 'Remote link fixture did not render'
        browser.click(150, 40)
        browser.drain(1)

        assert browser.title != 'Private local document', 'Remote link opened a local document'
        browser.send(20, url + '/redirect-local')
        browser.drain(2)

        assert browser.title != 'Private local document', 'HTTP redirect opened a cached local document'
        browser.send(20, local.as_uri())
        browser.ready('Private local document')

        assert browser.rgb(20, 20) == (255, 0, 0), 'Trusted address navigation was blocked'

        print('PASS browser file policy: cached images/frames/scripts, file base URLs, remote links and HTTP redirects denied; explicit local navigation works')
    finally:
        browser.close()
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == '__main__':
    main()
