"""Trusted-origin adapter for the unchanged upstream aurweb application."""
from contextvars import ContextVar
from urllib.parse import urlsplit
import os
from xml.sax.saxutils import escape

import aurweb.config
import aurweb.db

_origin = ContextVar('brine_request_origin', default=None)
_upstream_get = aurweb.config.get
_sessions = ContextVar('brine_request_sessions', default=None)
_upstream_session = aurweb.db.get_session


def request_session(engine=None):
    sessions = _sessions.get()
    if sessions is None:
        return _upstream_session(engine)
    name = aurweb.db.name()
    if name not in sessions:
        from sqlalchemy.orm import sessionmaker
        sessions[name] = sessionmaker(autocommit=True, autoflush=False,
                                     bind=engine or aurweb.db.get_engine())()
    return sessions[name]


def configured(section, option):
    value = _upstream_get(section, option)
    origin = _origin.get()
    if origin and section == 'options':
        if option == 'aur_location':
            return origin
        if option == 'git_clone_uri_anon':
            return origin+'/%s.git'
        if option == 'git_clone_uri_priv':
            return 'ssh://aur@'+urlsplit(origin).hostname+':'+os.environ.get('COAST_SSH_PORT','2222')+'/%s.git'
    return value


aurweb.config.get = configured
aurweb.db.get_session = request_session
from aurweb.asgi import app as upstream  # noqa: E402

# Access log URLs can contain one-time account tokens. Keep application/error
# logging, but never retain those request URLs in the container journal.
import logging  # noqa: E402
logging.getLogger('uvicorn.access').disabled = True


async def app(scope, receive, send):
    if scope['type'] != 'http':
        return await upstream(scope, receive, send)
    headers = {name.decode('latin1').lower(): value.decode('latin1')
               for name, value in scope['headers']}
    origin = scope.get('scheme', 'http')+'://'+headers.get('host', '')
    if scope['method']=='GET' and scope['path']=='/static/opensearch.xml':
        safe=escape(origin,{'"':'&quot;'})
        body=('<?xml version="1.0" encoding="UTF-8"?>'
              '<OpenSearchDescription xmlns="http://a9.com/-/spec/opensearch/1.1/">'
              '<ShortName>Brine</ShortName><Description>Search Brine community package names and descriptions.</Description>'
              '<InputEncoding>UTF-8</InputEncoding><Url type="text/html" template="'+safe+'/packages?K={searchTerms}"/>'
              '<Url rel="self" type="application/opensearchdescription+xml" template="'+safe+'/static/opensearch.xml"/>'
              '</OpenSearchDescription>').encode()
        await send(dict(type='http.response.start',status=200,headers=[
            (b'content-type',b'application/opensearchdescription+xml; charset=utf-8'),
            (b'content-length',str(len(body)).encode()),(b'cache-control',b'no-cache')]))
        return await send(dict(type='http.response.body',body=body))
    if scope['method'] not in ('GET', 'HEAD', 'OPTIONS'):
        candidate = headers.get('origin') or headers.get('referer', '')
        parsed = urlsplit(candidate)
        supplied = parsed.scheme+'://'+parsed.netloc
        if supplied != origin or headers.get('sec-fetch-site') == 'cross-site':
            body = b'An operation must originate from this Brine community site.\n'
            await send(dict(type='http.response.start', status=403,
                            headers=[(b'content-type', b'text/plain; charset=utf-8'),
                                     (b'content-length', str(len(body)).encode())]))
            return await send(dict(type='http.response.body', body=body))
    token = _origin.set(origin)
    sessions = {}
    session_token = _sessions.set(sessions)
    async def safe_send(message):
        if message['type']=='http.response.start':
            headers=[]
            for name,value in message.get('headers',[]):
                if name.lower()==b'set-cookie' and value.startswith((b'AURSID=',b'AURREMEMBER=')):
                    if b'httponly' not in value.lower():value+=b'; HttpOnly'
                headers.append((name,value))
            message=dict(message,headers=headers)
        await send(message)
    try:
        await upstream(scope, receive, safe_send)
    finally:
        for session in sessions.values():
            session.close()
        _sessions.reset(session_token)
        _origin.reset(token)
