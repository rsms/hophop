pub struct str {
	ptr *u8
	len uint
}

pub type rune u32

pub enum LogLevel i32 {
	Trace   = -20
	Debug   = -10
	Info    = 0
	Warning = 20
	Error   = 30
	Fatal   = 40
}

pub enum LoggerFlag u32 {
	Level     = 1 << 0
	Date      = 1 << 1
	Time      = 1 << 2
	ShortFile = 1 << 3
	LongFile  = 1 << 4
	Line      = 1 << 5
	Function  = 1 << 6
	Thread    = 1 << 7
	Styling   = 1 << 8
	Prefix    = 1 << 9
}

pub type LoggerFlags u32

pub type LogFlags LoggerFlags

pub struct Logger {
	handler   fn(&Logger, &str, LogLevel, LogFlags)
	min_level LogLevel
	flags     LogFlags
	prefix    &str
}

pub struct Allocator {
	impl fn(*Allocator, uint, uint, uint, *uint, u32) uint
}

pub struct Context {
	mem      *Allocator
	temp_mem *Allocator
	log      Logger
}

pub struct PrintContext {
	log Logger
}
