import "platform/playbit" { Err, HandleSelfThread, thread_log_write }

pub fn write(message &str) Err {
	return thread_log_write(HandleSelfThread, message, flags: 0)
}

pub fn write_flags(message &str, flags u32) Err {
	return thread_log_write(HandleSelfThread, message, flags)
}
