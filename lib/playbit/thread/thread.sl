import "platform/playbit" { Handle as PBHandle, thread_exit, thread_exit_process }

pub type Thread i32

pub const Self Thread = 1

pub const None Thread = 0

pub const SignalRunning u32 = 1 << 0

pub const SignalTerminated u32 = 1 << 1

pub const SignalWritable u32 = 1 << 2

pub const SignalReadable u32 = 1 << 3

pub fn exit(status i32) i32 {
	return thread_exit(1 as PBHandle, status) as i32
}

pub fn exit_process(status i32) i32 {
	return thread_exit_process(1 as PBHandle, status) as i32
}
