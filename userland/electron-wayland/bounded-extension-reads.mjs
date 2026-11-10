// Bound native handles used by the bundled extension metadata scanner.
// Its utility loader redirects `fs` to Electron's original-fs. Both objects
// share this queue; native reads, errors and permissions remain authoritative.
// Other files and write-capable opens retain their native APIs.
import fs from 'node:fs';
import originalFS from 'node:original-fs';
import { syncBuiltinESMExports, enableCompileCache, flushCompileCache } from 'node:module';

// Node validates the V8 version, flags and source before consuming bytecode.
// Retain it in the user's private cache for subsequent editor/helper launches.
if (process.env.NODE_COMPILE_CACHE) {
    const cache = enableCompileCache(process.env.NODE_COMPILE_CACHE);
    if (process.env.COAST_CODEOSS_QA === '1')
        console.error('COAST_COMPILE_CACHE', JSON.stringify(cache));
    if (cache.directory) setTimeout(() => flushCompileCache(), 5000).unref();
}


const metadata = /^\/usr\/share\/codeoss-workbench\/extensions\/[^/]+\/package(?:\.nls(?:\.[^/]+)?)?\.json$/;
const limit = 8, pending = [], leases = new Map(), patched = new WeakSet();
const manifests = new Set(), methods = {}, errorCodes = {};
const trace = process.env.COAST_METADATA_READ_TRACE === '1';
let active = 0, peak = 0, queuedPeak = 0, completed = 0, errors = 0;

function finish(lease) {
    if (lease.done) return;
    lease.done = true;
    if (leases.get(lease.fd) === lease) leases.delete(lease.fd);
    active--; completed++;
    if (pending.length) pending.shift()();
    else if (trace && active === 0) console.error('COAST_METADATA_READS', JSON.stringify({
        pid:process.pid, entry:process.env.VSCODE_ESM_ENTRYPOINT,
        completed, peak, queuedPeak, active, errors, methods, errorCodes, manifests:manifests.size
    }));
}
function enqueue(method, start) {
    const run = () => {
        active++; peak = Math.max(peak, active);
        methods[method] = (methods[method] ?? 0) + 1;
        start();
    };
    if (active < limit) run();
    else { pending.push(run); queuedPeak = Math.max(queuedPeak, pending.length); }
}
function outcome(error, path) {
    if (error) {
        errors++;
        const code = error.code ?? error.name;
        errorCodes[code] = (errorCodes[code] ?? 0) + 1;
    } else if (path.endsWith('/package.json')) manifests.add(path);
}
function readonly(flags) {
    return typeof flags === 'number' ? Number.isSafeInteger(flags) && flags >= 0 && flags <= 0xffffffff && (flags & 3) === 0 : flags === 'r' || flags === 'rs';
}
function install(api, label) {
    if (patched.has(api)) return;
    patched.add(api);
    const open = api.open, close = api.close, closeSync = api.closeSync;
    api.open = function (path, flags, ...args) {
        const callback = args.at(-1), mode = args.length === 2 ? args[0] : undefined;
        if (typeof path !== 'string' || !metadata.test(path) || !readonly(flags) ||
            typeof callback !== 'function' || args.length > 2 ||
            (mode !== undefined && (!Number.isInteger(mode) || mode < 0 || mode > 0xffffffff))) {
            return Reflect.apply(open, this, [path, flags, ...args]);
        }
        const receiver = this;
        enqueue(label+'.open', () => {
            const lease = {fd:undefined, done:false};
            const forwarded = [...args.slice(0,-1), (error, fd) => {
                outcome(error,path);
                if (error) finish(lease);
                else {lease.fd=fd; leases.set(fd,lease);}
                callback(error,fd);
            }];
            try {Reflect.apply(open,receiver,[path,flags,...forwarded]);}
            catch (error) {outcome(error,path);finish(lease);callback(error);}
        });
    };
    api.close = function (fd, callback) {
        const lease=leases.get(fd);
        if (!lease || typeof callback !== 'function') return Reflect.apply(close,this,arguments);
        return Reflect.apply(close,this,[fd,error => {
            if (!error) finish(lease);
            callback(error);
        }]);
    };
    api.closeSync = function (fd) {
        const lease=leases.get(fd), result=Reflect.apply(closeSync,this,arguments);
        if (lease) finish(lease);
        return result;
    };
}
install(fs,'fs');
install(originalFS,'original-fs');
syncBuiltinESMExports();
if (trace) console.error('COAST_METADATA_QUEUE',JSON.stringify({pid:process.pid,entry:process.env.VSCODE_ESM_ENTRYPOINT,limit}));
