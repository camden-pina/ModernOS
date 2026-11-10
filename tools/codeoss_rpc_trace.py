"""Add bounded request/ack/reply observation to the pinned workbench RPC logger.

Uses the existing logger seam; leaves serialization, dispatch and transport
unchanged. Only diagnostic images request this transformation.
"""
import re


def instrument_rpc(data):
    pattern = rb'this\._protocol=(\w+),this\._logger=(\w+),this\._uriTransformer=(\w+),this\._uriReplacer='
    matches = list(re.finditer(pattern, data))
    if len(matches) != 1:
        raise ValueError('Pinned RPC logger seam is missing or ambiguous')
    match = matches[0]
    logger = match[2]
    factory = rb'''((original)=>{
        const instance=globalThis.__coastRPCInstance=(globalThis.__coastRPCInstance??0)+1;
        const tracked=new Set();
        let sink;
        if(typeof __coastRpcFS!=='undefined') {
            try {
                sink=__coastRpcFS.openSync('/home/port-test/runtime/coast-rpc-'+process.pid+'.jsonl','a',0o600);
                process.once('exit',()=>__coastRpcFS.closeSync(sink));
            } catch {}
        }
        let records=0;
        const emit=(direction,bytes,id,side,label,details)=>{
            const key=side+':'+id;
            if(String(label).startsWith('request: ') || String(label).startsWith('receiveRequest ')) {
                if(!/(?:Commands|SCM|QuickOpen)\./.test(label))return;
                if(/\.(?:\$registerCommand|\$unregisterCommand|\$fireCommandActivationEvent)\(/.test(label))return;
                tracked.add(key);
            } else if(!tracked.has(key))return;
            if(records++>=2048)return;
            try {
                const node=typeof __coastRpcFS!=='undefined';
                const record={instance,counter:records,time:Date.now(),
                    pid:globalThis.process?.pid??0,role:node?'node':'renderer',
                    direction,bytes,id,side,label:String(label).slice(0,200),
                    arg0:Array.isArray(details)&&typeof details[0]==='string'?details[0].slice(0,100):undefined};
                // Keep observation out of both RPC console redirection and
                // libuv's batched stderr writes. Each file append is bounded.
                if(node&&sink!==undefined)__coastRpcFS.writeSync(sink,JSON.stringify(record)+'\n');
                else {
                    // Keep renderer evidence independent of native console
                    // rate limits. The existing DOM observer reads this ring.
                    const ring=globalThis.__coastRPCTail??(globalThis.__coastRPCTail=[]);
                    ring.push(record);if(ring.length>128)ring.shift();
                }
            } catch {}
        };
        return {
            logIncoming(...args){emit('receive',...args);return original?.logIncoming(...args)},
            logOutgoing(...args){emit('send',...args);return original?.logOutgoing(...args)}
        };
    })(''' + logger + b')'
    replacement = match[0].replace(b'this._logger=' + logger + b',', b'this._logger=' + factory + b',')
    result = data[:match.start()] + replacement + data[match.end():]
    if b'process.parentPort.on("message"' in data:
        result = b"import * as __coastRpcFS from 'node:fs';\n" + result
    return result


def instrument_wire(data):
    """Observe the native port callbacks separately from the RPC dispatcher."""
    node = b'process.parentPort.on("message"' in data
    pattern = (rb'(\w+)\.on\("message",(\w+)=>(\w+)\.fire\((\w+)\.wrap\(\2\.data\)\)\)' if node else
               rb'(\w+)\.onmessage=\((\w+)=>\{\2\.data&&(\w+)\.fire\((\w+)\.wrap\(\2\.data\)\)\}\)')
    matches=list(re.finditer(pattern,data))
    if len(matches)!=1:raise ValueError('Pinned native MessagePort receive seam is missing or ambiguous')
    m=matches[0]
    original=m[3]+b'.fire('+m[4]+b'.wrap('+m[2]+b'.data))'
    replacement=m[0].replace(original,b"(__coastWireMark('receive',"+m[2]+b'.data),'+original+b')')
    data=data[:m.start()]+replacement+data[m.end():]
    pattern=rb'send:(\w+)=>(\w+)\.postMessage\(\1\.buffer\)'
    matches=list(re.finditer(pattern,data))
    if len(matches)!=1:raise ValueError('Pinned native MessagePort send seam is missing or ambiguous')
    m=matches[0];buf=m[1]+b'.buffer'
    replacement=b'send:'+m[1]+b"=>(__coastWireMark('send',"+buf+b'),'+m[2]+b'.postMessage('+buf+b"),__coastWireMark('sent',"+buf+b'))'
    data=data[:m.start()]+replacement+data[m.end():]
    prelude=b'''let __coastWireFD, __coastWireCount=0;
function __coastWireMark(direction,bytes) {
    if(direction==='send')return;
    if(__coastWireCount++>=32768)return;
    try {
        const node=typeof __coastRpcFS!=='undefined';
        const record={wire:true,counter:__coastWireCount,time:Date.now(),direction,
            bytes:bytes.byteLength,head:Array.from(bytes.subarray(0,8))};
        if(node) {
            __coastWireFD??=__coastRpcFS.openSync('/home/port-test/runtime/coast-rpc-'+process.pid+'-wire.jsonl','a',0o600);
            __coastRpcFS.writeSync(__coastWireFD,JSON.stringify(record)+'\\n');
        } else {
            const ring=globalThis.__coastRPCWire??(globalThis.__coastRPCWire=[]);
            ring.push(record);if(ring.length>64)ring.shift();
        }
    } catch {}
}
'''
    return prelude+data
