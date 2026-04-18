// Verifies runtime behavior for compiler diagnostic nonconst if.
import "compiler"

fn runtime_nonconst_if(flag bool) {
	if flag {
		compiler.error("runtime-dependent")
	}
}

fn main() {
	runtime_nonconst_if(false)
}
