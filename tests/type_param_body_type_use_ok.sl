// Verifies type parameter body type use is accepted.
fn body_uses_type_param(T type) {
	var p *T
	_ = p
}
