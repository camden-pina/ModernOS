#!/usr/bin/python
"""Apply a small, auditable Brine identity overlay; preserve upstream credits."""
from pathlib import Path
import shutil

root=Path('/opt/aurweb')
runtime=Path('/opt/coast')
templates=root/'templates'
head=templates/'partials/head.html'
head.write_text(head.read_text().replace('<title>AUR (','<title>Brine Community (').replace(
    '<!-- Resources -->','<link rel="stylesheet" href="/static/css/brine.css">\n    <!-- Resources -->').replace(
    'Arch Linux User Repository (AUR) Package Search','Brine Community Package Search').replace(
    '/static/images/favicon.ico','/static/images/brine.svg'))
nav=templates/'partials/archdev-navbar.html'
nav.write_text(nav.read_text().replace('AUR {% trans %}Home','Brine {% trans %}Home'))
(templates/'partials/navbar.html').write_text('''<header id="brine-header">
<a class="brine-wordmark" href="/">Brine<span>Community packages</span></a>
<nav aria-label="Resources"><a href="/packages">Browse packages</a><a href="/guidelines">Contribute</a><a href="/rpc">API</a></nav>
</header>''')
shutil.copyfile(runtime/'home.html',templates/'home.html')
shutil.copyfile(runtime/'brine.css',root/'static/css/brine.css')
shutil.copyfile(runtime/'brine.svg',root/'static/images/brine.svg')
feed=root/'aurweb/routers/rss.py'
feed.write_text(feed.read_text().replace('AUR Newest Packages','Brine Community Newest Packages').replace(
    'The latest and greatest packages in the AUR','Recent Brine community package submissions').replace(
    '/static/css/archnavbar/aurlogo.png','/static/images/brine.svg').replace(
    'lambda_cache("rss",','lambda_cache("rss:"+request.url.scheme+"://"+request.url.netloc,'))
footer=templates/'partials/footer.html'
footer.write_text(footer.read_text().replace(
    '<p>Report issues <a href="https://gitlab.archlinux.org/archlinux/aurweb/-/issues/new?issuable_template=Bug">here</a>.</p>',
    '<p>Brine Community runs the open source aurweb software. <a href="/guidelines">Contribution and security guide</a>.</p>').replace(
    'AUR packages are user produced content. Any use of the provided files is at your own risk.',
    'Community recipes are submitted by users. Review their source and install scripts before building.'))
