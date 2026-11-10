"""Bounded, non-executing AUR v5 metadata reader for Coast's community service."""
import gzip
import io
import json
import re
from urllib.parse import urlencode

NAME = re.compile(r'[a-zA-Z0-9][a-zA-Z0-9@._+-]{0,63}\Z')
MAX_JSON = 16 * 1024 * 1024


def metadata(value):
    if not isinstance(value, dict) or any(not isinstance(value.get(key),str) or
       not NAME.fullmatch(value[key]) for key in ('Name','PackageBase')):
        raise ValueError('invalid community package identity')
    result = {key:value[key] for key in ('Name','PackageBase')}
    for key in ('Version','Description','Maintainer','URL'):
        text = value.get(key) or ''
        if not isinstance(text,str) or len(text)>4096 or any(ord(c)<32 for c in text):
            raise ValueError('invalid community metadata field: '+key)
        result[key] = text
    if not result['Version']:
        raise ValueError('community package has no version')
    for key in ('Depends','MakeDepends','CheckDepends','OptDepends','Provides','Conflicts','Replaces','License','Keywords'):
        items = value.get(key) or []
        if not isinstance(items,list) or len(items)>1024 or any(not isinstance(v,str) or len(v)>4096 or
           any(ord(c)<32 for c in v) for v in items):
            raise ValueError('invalid community metadata list: '+key)
        result[key] = items
    result.update(Repository='community',Community=True,Commands=[])
    for key in ('NumVotes','Popularity','OutOfDate','FirstSubmitted','LastModified'):
        result[key] = value.get(key)
    return result


def rpc(fetch,kind,values):
    query=[('arg[]',name) for name in values] if kind=='info' else [('arg',values[0]),('by','name-desc')]
    data=json.loads(fetch('/rpc/v5/'+kind+'?'+urlencode(query),MAX_JSON))
    if not isinstance(data,dict):raise ValueError('invalid community RPC response')
    if data.get('version')!=5 or data.get('type')=='error':
        raise ValueError('community RPC: '+str(data.get('error','invalid response'))[:200])
    if not isinstance(data.get('results'),list) or len(data['results'])>10000:
        raise ValueError('invalid community result list')
    return [metadata(value) for value in data['results']]


def info(fetch,names):
    if not names:return {}
    if any(not isinstance(name,str) or not NAME.fullmatch(name) for name in names):
        raise ValueError('community lookup requires exact package names')
    result={}
    for start in range(0,len(names),100):
        requested=names[start:start+100]
        for package in rpc(fetch,'info',requested):
            if package['Name'] not in requested or package['Name'] in result:
                raise ValueError('unexpected or duplicate community package')
            result[package['Name']]=package
    return result


def search(fetch,queries):
    terms=[term for query in queries for term in query.split()]
    longest=max(terms,key=len,default='')
    if len(longest)>=2:
        return rpc(fetch,'search',[longest])
    # Upstream v5 requires two characters. Its public metadata index lets
    # Coast also provide literal one-letter and empty-query searches.
    compressed=fetch('/packages-meta-v1.json.gz',8*1024*1024)
    with gzip.GzipFile(fileobj=io.BytesIO(compressed)) as stream:
        raw=stream.read(MAX_JSON+1)
    if len(raw)>MAX_JSON:raise ValueError('community metadata index is too large')
    data=json.loads(raw)
    if not isinstance(data,list) or len(data)>10000:raise ValueError('invalid community metadata index')
    return [metadata(value) for value in data]
