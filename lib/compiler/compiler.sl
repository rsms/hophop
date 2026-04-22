import "builtin"

pub fn error(message &str) {}

pub fn error_at(location builtin.SourceLocation, message &str) {}

pub fn warn(message &str) {}

pub fn warn_at(location builtin.SourceLocation, message &str) {}
