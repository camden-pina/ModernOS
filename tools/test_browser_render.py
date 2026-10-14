#!/usr/bin/env python3
"""Pixel regressions using the complete, real NetSurf process."""
from browser_protocol import Browser, ROOT


def main():
    work = ROOT / 'build/browser-render-test'
    work.mkdir(parents=True, exist_ok=True)
    svg = work / 'paths.svg'
    svg.write_text('''<svg xmlns="http://www.w3.org/2000/svg" width="400" height="200" viewBox="0 0 200 100">
<path fill="#ff0000" d="M10 10 H90 V90 H10 Z M30 30 V70 H70 V30 Z"/>
<path fill="#0000ff" d="M110 90 C110 10 190 10 190 90 Z"/>
<path fill="none" stroke="#00aa00" stroke-width="3" d="M110 10 L190 10 L190 30"/>
</svg>''')
    tall = work / 'tall.html'
    tall.write_text('<!doctype html><title>Path and scroll regression</title>'
                    '<body style="margin:0;background:white"><img width="400" height="200" src="paths.svg">'
                    '<a href="short.html" style="display:block;position:fixed;z-index:1;left:0;top:240px;width:400px;height:30px;background:#00ff00">Fixed link</a>'
                    '<div style="height:1800px;background:#dddddd"></div></body>')
    short = work / 'short.html'
    short.write_text('<!doctype html><title>Short document regression</title>'
                     '<body style="margin:0;background:white"><div style="width:200px;height:60px;background:#ff0000"></div></body>')
    stacked = work / 'stacked.html'
    stacked.write_text('<!doctype html><title>Stacked links regression</title><body style="margin:0;background:white">'
                       '<a href="short.html" style="display:block;position:absolute;z-index:4;left:20px;top:20px;width:200px;height:80px;background:#00ff00">Front link</a>'
                       '<a href="tall.html" style="display:block;position:absolute;z-index:2;left:20px;top:20px;width:200px;height:80px;background:#0000ff">Covered link</a>'
                       '<div style="position:relative;margin-top:1100px;height:800px">'
                       '<a href="short.html" style="display:block;position:fixed;right:20px;bottom:20px;width:100px;height:40px;background:#ffff00">Bottom link</a></div></body>')
    browser = Browser(work, tall.as_uri())
    try:
        browser.ready('Path and scroll regression')
        browser.screenshot('svg-before-scroll')
        assert browser.rgb(40, 40) == (255, 0, 0), 'SVG fill is missing'
        assert browser.rgb(100, 100) == (255, 255, 255), 'SVG contour hole was filled'
        assert browser.rgb(300, 120) == (0, 0, 255), 'SVG cubic curve is missing'
        assert browser.rgb(300, 20) == (0, 170, 0), 'Open SVG stroke is missing'
        assert browser.rgb(300, 18) == (0, 170, 0), 'SVG stroke width was ignored'
        assert browser.rgb(200, 250) == (0, 255, 0), 'Fixed element is missing'

        browser.key(281)
        browser.screenshot('svg-after-scroll')

        assert browser.rgb(200, 250) == (0, 255, 0), 'Fixed element moved during scrolling'
        browser.click(200, 250)
        browser.ready('Short document regression', timeout=5)
        browser.screenshot('short-before-scroll')

        assert browser.rgb(20, 20) == (255, 0, 0)
        browser.key(281)
        browser.screenshot('short-after-scroll')

        assert browser.rgb(20, 20) == (255, 0, 0), 'Short page scrolled to a negative origin'
        browser.send(18, width=900, height=600)
        browser.drain(1)
        browser.screenshot('resized-short')

        assert (browser.width, browser.height) == (900, 600)
        assert browser.rgb(20, 20) == (255, 0, 0) and browser.rgb(220, 20) == (255, 255, 255)
        browser.navigate(stacked.as_uri(), 'Stacked links regression')
        browser.screenshot('stacked-before-scroll')

        assert browser.rgb(150, 50) == (0, 255, 0), 'Positioned siblings ignored z-index'

        # The native framebuffer frontend reserves 18 px for its scrollbars.
        fixed_x, fixed_y = browser.width - 18 - 70, browser.height - 18 - 30
        assert browser.rgb(fixed_x, fixed_y) == (
            255, 255, 0), 'Nested fixed element used the document instead of the viewport'
        browser.click(150, 50)
        browser.ready('Short document regression', timeout=5)
        browser.navigate(stacked.as_uri(), 'Stacked links regression')
        browser.key(281)
        browser.key(281)
        browser.key(281)

        assert browser.rgb(fixed_x, fixed_y) == (
            255, 255, 0), 'Nested bottom element moved during scrolling'
        browser.click(fixed_x, fixed_y)
        browser.ready('Short document regression', timeout=5)

        print('PASS browser pixels: SVG paths/curves/holes/strokes; fixed layout and clicks; sibling stacking and hit testing; short-page bounds; resize')
    finally:
        browser.close()


if __name__ == '__main__':
    main()
