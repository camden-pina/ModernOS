// Real JavaScriptCore shell fixture: no browser globals or DOM shims.
function assert(value, message) {
    if (!value)
        throw new Error(`FAIL JavaScriptCore: ${message}`);
}
class Counter {
    #value = 0;
    add(value = 1) {
        return this.#value += value;
    }
}
const counter = new Counter();
assert(counter.add(4) === 4 && counter.add() === 5, "private class fields");
const object = {
    nested: {value: 23}
};
assert(object?.nested?.value === 23 && (object.missing?.value ?? 17) === 17, "optional chaining");
assert([...new Set([2, 1, 2])].toSorted().join() === "1,2", "Set and immutable sorting");
assert(new Map([["key", 37]]).get("key") === 37, "Map");
assert(1234567890123456789n * 7n === 8641975230864197523n, "BigInt arithmetic");
assert(/\p{Letter}+/u.test("Ωλ"), "Unicode regular expressions");
const buffer = new ArrayBuffer(16);
const view = new DataView(buffer);
view.setBigUint64(0, 0x1122334455667788n, true);
assert(view.getBigUint64(0, true) === 0x1122334455667788n, "typed binary data");
assert(new Intl.NumberFormat("en-US").format(1234.5) === "1,234.5", "real ICU data");
async function* values() {
    yield 3;
    await Promise.resolve();
    yield 5;
}
async function checkJobs() {
    let total = 0;
    for await (const value of values())
        total += value;
    const result = await Promise.all([Promise.resolve(total), Promise.resolve(11)]);
    assert(result.join() === "8,11", "promises and async generators");
    // Exercise allocations and real collection while threads are available.
    for (let round = 0; round < 10; round++) {
        const objects = Array.from({length: 1500}, (_, index) => ({index, text: `row-${index}`}));
        assert(objects[1499].text === "row-1499", "allocated objects");
        gc();
    }
    print("JAVASCRIPTCORE CHECK PASS");
}
checkJobs().catch(error => {
    print(String(error));
    quit(1);
});
