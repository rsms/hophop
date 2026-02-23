pub enum PlatformOps i32 {
    NONE = 0
    PANIC = 1
    CONSOLE_LOG = 2
    EXIT = 3
}

pub fn exit(status i32) {}
pub fn console_log(msg str, flags u64) {}
