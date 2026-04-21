import "playbit/object" { ObserveAdd, ObserveOnce, observe as object_observe, signal as object_signal }

pub fn observe(handle i32, signals u32) i32 {
	return object_observe(handle, signals, flags: 0)
}

pub fn observe_once(handle i32, signals u32) i32 {
	return object_observe(handle, signals, flags: 1)
}

pub fn signal(handle i32, disableUser, enableUser, pulseUser u32) i32 {
	return object_signal(handle, disableUser, enableUser, pulseUser)
}
