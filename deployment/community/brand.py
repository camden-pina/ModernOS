#!/usr/bin/python
"""Apply a small, auditable Coast identity overlay; preserve upstream credits."""
from pathlib import Path
import shutil

root=Path('/opt/aurweb')
runtime=Path('/opt/coast')
templates=root/'templates'
head=templates/'partials/head.html'
head.write_text(head.read_text().replace('<title>AUR (','<title>Coast Community (').replace(
    '<!-- Resources -->','<link rel="stylesheet" href="/static/css/coast.css">\n    <!-- Resources -->').replace(
    'Arch Linux User Repository (AUR) Package Search','Coast Community Package Search').replace(
    '/static/images/favicon.ico','/static/images/coast.svg'))
nav=templates/'partials/archdev-navbar.html'
nav.write_text(nav.read_text().replace('AUR {% trans %}Home','Coast {% trans %}Home'))
(templates/'partials/navbar.html').write_text('''<header id="coast-header">
<a class="coast-wordmark" href="/">Coast<span>Community packages</span></a>
<nav aria-label="Resources"><a href="/packages">Browse packages</a><a href="/guidelines">Contribute</a><a href="/rpc">API</a></nav>
</header>''')
shutil.copyfile(runtime/'home.html',templates/'home.html')
shutil.copyfile(runtime/'coast.css',root/'static/css/coast.css')
shutil.copyfile(runtime/'coast.svg',root/'static/images/coast.svg')
feed=root/'aurweb/routers/rss.py'
feed.write_text(feed.read_text().replace('AUR Newest Packages','Coast Community Newest Packages').replace(
    'The latest and greatest packages in the AUR','Recent Coast community package submissions').replace(
    '/static/css/archnavbar/aurlogo.png','/static/images/coast.svg').replace(
    'lambda_cache("rss",','lambda_cache("rss:"+request.url.scheme+"://"+request.url.netloc,'))
footer=templates/'partials/footer.html'
footer.write_text(footer.read_text().replace(
    '<p>Report issues <a href="https://gitlab.archlinux.org/archlinux/aurweb/-/issues/new?issuable_template=Bug">here</a>.</p>',
    '<p>Coast runs the open source aurweb community software. <a href="/guidelines">Coast contribution and security guide</a>.</p>').replace(
    'AUR packages are user produced content. Any use of the provided files is at your own risk.',
    'Community recipes are submitted by users. Review their source and install scripts before building.'))
