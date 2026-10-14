import {Value, compute} from "./feature-module.mjs";
const value = new Value(30);
if (await compute(value.value) !== 37)
    throw new Error("FAIL module execution");
print("JAVASCRIPTCORE MODULE PASS");
