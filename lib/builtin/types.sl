pub struct str {
	ptr *u8
	len int
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

pub enum LogFlag u32 {
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

pub type LogFlags u32

pub struct SourceLocation {
	file         &str
	start_line   int
	start_column int
	end_line     int
	end_column   int
}

pub struct Logger {
	handler   fn(&Logger, &str, LogLevel, LogFlags)
	min_level LogLevel
	flags     LogFlags
	prefix    &str
}

pub struct MemAllocator {
	handler fn(*MemAllocator, rawptr, int, int, *int, u32, SourceLocation) rawptr
}

pub struct Context {
	allocator      MemAllocator
	temp_allocator MemAllocator
	logger         Logger
	user1          rawptr
	user2          rawptr
	_reserved      rawptr
	deadline       u64
}

pub struct PrintContext {
	logger Logger
}
