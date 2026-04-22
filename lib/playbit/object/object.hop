import "platform/playbit" { Handle as PBHandle, Signals as PBSignals, object_observe, object_signal }

pub const ObserveOnce u32 = 1 << 0

pub const ObserveAdd u32 = 1 << 1

pub fn observe(handle i32, signals, flags u32) i32 {
	var h PBHandle  = handle as PBHandle
	var s PBSignals = signals as PBSignals
	return object_observe(h, signals: s, flags) as i32
}

pub fn signal(handle i32, disableUser, enableUser, pulseUser u32) i32 {
	var h       PBHandle  = handle as PBHandle
	var disable PBSignals = disableUser as PBSignals
	var enable  PBSignals = enableUser as PBSignals
	var pulse   PBSignals = pulseUser as PBSignals
	return object_signal(h, disableUser: disable, enableUser: enable, pulseUser: pulse) as i32
}
