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
            panic(ptr, len) {
                const mem = instance.exports.memory;
                if (!(mem instanceof WebAssembly.Memory)) {
                    fail("wasm-min: module does not export memory");
                }
                const bytes = new Uint8Array(mem.buffer, ptr >>> 0, len >>> 0);
                const text = Buffer.from(bytes).toString("utf8");
                process.stderr.write(`wasm-min: panic: ${text}\n`);
                process.exit(1);
            },
        },
    });
} catch (err) {
    fail(`wasm-min: failed to instantiate module: ${err.message}`);
}

const entry = instance.exports.sl_main;
if (typeof entry !== "function") {
    fail("wasm-min: module does not export sl_main");
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
    fail("wasm-min: sl_main returned a non-integer result");
}
process.exit(result);
