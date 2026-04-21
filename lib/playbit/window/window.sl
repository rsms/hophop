import "platform/playbit" { Handle as PBHandle, WindowConfig as PBWindowConfig, WindowInfo as PBWindowInfo, handle_close, thread_window_create, window_frame_sync_enable, window_info_get, window_set_title }
import "playbit/handle"   { find_by_object_type }

const ObjectTypeWindow u16 = 6

pub const None i32 = 0

pub const SignalResize u32 = 1 << 0

pub const SignalMove u32 = 1 << 1

pub const SignalFrameSync u32 = 1 << 2

pub const SignalRepaint u32 = 1 << 3

pub const SignalKey u32 = 1 << 4

pub fn main() i32 {
	return find_by_object_type(ObjectTypeWindow)
}

pub fn close(window i32) i32 {
	return handle_close(window as PBHandle) as i32
}

pub fn open(title &str, width, height f32) i32 {
	var bytes &[u8] = title
	var data  *u8   = (null as rawptr) as *u8
	if len(bytes) > 0 {
		data = &bytes[0]
	}
	var wc PBWindowConfig = {
		x:        -1 as f32,
		y:        -1 as f32,
		width,
		height,
		title:    data,
		titleLen: len(bytes) as u32,
	}
	var config            = &wc
	return thread_window_create(1 as PBHandle, config) as i32
}

fn read_info(window i32, info *PBWindowInfo) i32 {
	return window_info_get(window as PBHandle, info) as i32
}

pub fn width(window i32) f32 {
	var info PBWindowInfo
	_ = read_info(window, info: &info)
	return info.width
}

pub fn height(window i32) f32 {
	var info PBWindowInfo
	_ = read_info(window, info: &info)
	return info.height
}

pub fn scale(window i32) f32 {
	var info PBWindowInfo
	_ = read_info(window, info: &info)
	return info.dpScale
}

pub fn set_title(window i32, title &str) i32 {
	return window_set_title(window as PBHandle, title) as i32
}

pub fn frame_sync_enable(window i32, enable bool) i32 {
	return window_frame_sync_enable(window as PBHandle, flags: enable as u64) as i32
}
