// Upstream JSC shell agents run separate real VMs on native pthreads.
// They are not browser Web Workers or separate sandboxed site processes.
function assert(value, message) {
    if (!value)
        throw new Error(`FAIL JSC agents: ${message}`);
}
const agents = 4;
const shared = new SharedArrayBuffer(4 * Int32Array.BYTES_PER_ELEMENT);
const data = new Int32Array(shared);
for (let id = 0; id < agents; ++id) {
    $.agent.start(`
        $.agent.receiveBroadcast(shared => {
            const data = new Int32Array(shared);
            Atomics.add(data, 1, 1);
            Atomics.notify(data, 1);
            Atomics.wait(data, 0, 0, 20000);
            if (Atomics.load(data, 0) !== 1) throw new Error("agent start timeout");
            for (let round = 0; round < 10; ++round) {
                const objects = Array.from({length: 1000}, (_, i) => ({id: ${id}, i, text: "item-" + i}));
                if (objects[999].i !== 999) throw new Error("agent heap corruption");
                for (let i = 0; i < 100; ++i) Atomics.add(data, 3, 1);
                gc();
            }
            Atomics.add(data, 2, 1);
            Atomics.notify(data, 2);
            $.agent.report("finished-${id}");
            $.agent.leaving();
        });
    `);
}
$.agent.broadcast(shared, 0);
const deadline = Date.now() + 20000;
function waitFor(index, expected) {
    while (Atomics.load(data, index) !== expected) {
        assert(Date.now() < deadline, `wait ${index} timed out`);
        Atomics.wait(data, index, Atomics.load(data, index), 50);
    }
}
waitFor(1, agents);
Atomics.store(data, 0, 1);
Atomics.notify(data, 0, agents);
waitFor(2, agents);
assert(Atomics.load(data, 3) === agents * 1000, "shared atomic counter");
const reports = new Set();
while (reports.size < agents) {
    assert(Date.now() < deadline, "reports timed out");
    const report = $.agent.getReport();
    if (report !== null)
        reports.add(report);
    else
        $.agent.sleep(1);
}
for (let id = 0; id < agents; ++id)
    assert(reports.has(`finished-${id}`), "agent report");
print("JAVASCRIPTCORE AGENTS PASS");
