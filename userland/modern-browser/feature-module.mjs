export class Value {
    #value;
    constructor(value) {
        this.#value = value;
    }
    get value() {
        return this.#value;
    }
}
export const compute = async value => (await Promise.resolve(value)) + 7;
