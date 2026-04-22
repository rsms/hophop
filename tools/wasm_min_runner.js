const fs = require("fs");

function fail(message) {
    process.stderr.write(`${message}\n`);
    process.exit(1);
}

if (process.argv.length !== 3) {
    fail("usage: node tools/wasm_min_runner.js <module.wasm>");
}

const wasmPath = process.argv[2];
let bytes;
try {
    bytes = fs.readFileSync(wasmPath);
} catch (err) {
    fail(`wasm-min: failed to read module: ${err.message}`);
}

let moduleObject;
try {
    moduleObject = new WebAssembly.Module(bytes);
} catch (err) {
    fail(`wasm-min: failed to compile module: ${err.message}`);
}

let instance;
try {
    instance = new WebAssembly.Instance(moduleObject, {
        wasm_min: {
            exit(status) {
                process.exit(status | 0);
            },
            console_log(ptr, len, flags) {
                const mem = instance.exports.memory;
                if (!(mem instanceof WebAssembly.Memory)) {
                    fail("wasm-min: module does not export memory");
                }
                const bytes = new Uint8Array(mem.buffer, ptr >>> 0, len >>> 0);
                const text = Buffer.from(bytes).toString("utf8");
                void flags;
                process.stdout.write(`${text}\n`);
            },
            panic(ptr, len, flags) {
                const mem = instance.exports.memory;
                if (!(mem instanceof WebAssembly.Memory)) {
                    fail("wasm-min: module does not export memory");
                }
                const bytes = new Uint8Array(mem.buffer, ptr >>> 0, len >>> 0);
                const text = Buffer.from(bytes).toString("utf8");
                void flags;
                process.stderr.write(`wasm-min: panic: ${text}\n`);
                process.exit(1);
            },
        },
    });
} catch (err) {
    fail(`wasm-min: failed to instantiate module: ${err.message}`);
}

function callNamedExport(name) {
    const fn = instance.exports[name];
    if (typeof fn !== "function") {
        return undefined;
    }
    let result;
    try {
        result = fn();
    } catch (err) {
        fail(`wasm-min: trapped in ${name}: ${err.message}`);
    }
    if (typeof result === "undefined") {
        process.stdout.write(`wasm-min: ${name} returned void\n`);
        return 0;
    }
    if (typeof result !== "number" || !Number.isInteger(result)) {
        fail(`wasm-min: ${name} returned a non-integer result`);
    }
    process.stdout.write(`wasm-min: ${name} returned ${result}\n`);
    return result | 0;
}

function callStringExport(name, text) {
    const fn = instance.exports[name];
    if (typeof fn !== "function") {
        return undefined;
    }
    const mem = instance.exports.memory;
    if (!(mem instanceof WebAssembly.Memory)) {
        fail("wasm-min: module does not export memory");
    }
    const encoded = Buffer.from(text, "utf8");
    const ptr = (mem.buffer.byteLength - encoded.length) >>> 0;
    const bytes = new Uint8Array(mem.buffer);
    bytes.set(encoded, ptr);
    let result;
    try {
        result = fn(ptr, encoded.length);
    } catch (err) {
        fail(`wasm-min: trapped in ${name}: ${err.message}`);
    }
    if (typeof result === "undefined") {
        process.stdout.write(`wasm-min: ${name} returned void\n`);
        return 0;
    }
    if (typeof result !== "number" || !Number.isInteger(result)) {
        fail(`wasm-min: ${name} returned a non-integer result`);
    }
    process.stdout.write(`wasm-min: ${name} returned ${result}\n`);
    return result | 0;
}

callStringExport("hop_test_str", "hello from js");
callNamedExport("hop_test");

const entry = instance.exports.hop_main;
if (typeof entry !== "function") {
    fail("wasm-min: module does not export hop_main");
}

let result;
try {
    result = entry();
} catch (err) {
    fail(`wasm-min: trapped: ${err.message}`);
}

if (typeof result === "undefined") {
    process.exit(0);
}
if (typeof result !== "number" || !Number.isInteger(result)) {
    fail("wasm-min: hop_main returned a non-integer result");
}
process.exit(result);
