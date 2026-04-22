pub fn log(message &str, level LogLevel, flags LogFlags) {
	context.logger.handler(&context.logger, message, level, flags)
}

pub fn log_trace(message &str) {
	log(message, level: LogLevel.Trace, flags: 0 as LogFlags)
}

pub fn log_debug(message &str) {
	log(message, level: LogLevel.Debug, flags: 0 as LogFlags)
}

pub fn log_info(message &str) {
	log(message, level: LogLevel.Info, flags: 0 as LogFlags)
}

pub fn log_warn(message &str) {
	log(message, level: LogLevel.Warning, flags: 0 as LogFlags)
}

pub fn log_error(message &str) {
	log(message, level: LogLevel.Error, flags: 0 as LogFlags)
}

pub fn log_fatal(message &str) {
	log(message, level: LogLevel.Fatal, flags: 0 as LogFlags)
}
