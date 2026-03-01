import "reflect"

pub fn error(message &str) {}

pub fn error_at(span &reflect.Span, message &str) {}

pub fn warn(message &str) {}

pub fn warn_at(span &reflect.Span, message &str) {}
