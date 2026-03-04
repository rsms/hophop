fn helper_used() {}

fn helper_unused() {}

pub fn template_root(fmt &str, args ...anytype) {
	if len(fmt) > 0 {
		helper_used()
	}
	_ = len(args)
}
