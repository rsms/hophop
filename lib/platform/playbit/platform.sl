fn sl_entry_main()

pub type Handle i32

pub type Rights u32

pub type Signals u32

pub type ObjectId u32

pub type HandleName u16

pub type Time u64

pub type Duration i64

pub type Date i64

pub const HandleInvalid Handle = 0

pub const HandleSelfThread Handle = 1

pub const EventSizeMax uint = 16384

pub enum Err i32 {
	None           = 0
	Invalid        = -1
	SysOp          = -2
	NoMem          = -3
	BadHandle      = -4
	BadName        = -5
	NotFound       = -6
	NameTooLong    = -7
	Canceled       = -8
	NotSupported   = -9
	Exists         = -10
	End            = -11
	AccessDenied   = -12
	Again          = -13
	Deferred       = -14
	Already        = -15
	IoErr          = -16
	BadAddress     = -17
	ShouldWait     = -18
	Timeout        = -19
	BufferTooSmall = -20
	Unknown        = -2000000000
}

pub enum ObjectType u16 {
	None       = 0
	Clock      = 1
	Stream     = 2
	Textplan   = 3
	Thread     = 4
	Window     = 6
	Channel    = 7
	Gui        = 8
	Net        = 9
	NetSession = 10
	Texture    = 13
}

pub enum Right u32 {
	None          = 0
	Transfer      = 1 << 0
	Duplicate     = 1 << 1
	Read          = 1 << 4
	Write         = 1 << 5
	ManageProcess = 1 << 6
	ManageThread  = 1 << 7
	Network       = 1 << 8
	Observe       = 1 << 9
	Signal        = 1 << 10
	SameRights    = 1 << 31
}

pub enum HandleNameId u16 {
	None    = 0
	Console = 1
}

pub enum HandleListFilter u8 {
	ByName       = 1
	ByObjectId   = 2
	ByObjectType = 3
}

pub enum PointerKind u8 {
	Mouse    = 1
	Touch    = 2
	Trackpad = 3
	Pen      = 4
}

pub enum PointerFlag u16 {
	Primary   = 1 << 0
	InContact = 1 << 1
	Eraser    = 1 << 2
	Inverted  = 1 << 3
	Coalesced = 1 << 4
	Predicted = 1 << 5
}

pub enum ScrollPhase u8 {
	Changed  = 0
	Began    = 1
	Ended    = 2
	Momentum = 3
}

pub enum ScrollFlag u16 {
	Precise   = 1 << 0
	Inverted  = 1 << 1
	UnitLines = 1 << 2
	UnitPages = 1 << 3
}

pub enum GesturePhase u8 {
	Changed = 0
	Began   = 1
	Ended   = 2
}

pub enum KeyboardFlag u16 {
	Repeat = 1 << 0
}

pub enum KeyboardModifier u16 {
	Shift    = 1 << 0
	Ctrl     = 1 << 1
	Alt      = 1 << 2
	Meta     = 1 << 3
	CapsLock = 1 << 4
	Fn       = 1 << 5
}

pub enum KeyboardKey u16 {
	None      = 0x0000
	Escape    = 0x238B
	Enter     = 0x23CE
	Tab       = 0x21E5
	Backspace = 0x232B
	Insert    = 0x2380
	Delete    = 0x2326
	Left      = 0x2190
	Right     = 0x2192
	Down      = 0x2193
	Up        = 0x2191
	PageUp    = 0x21DE
	PageDown  = 0x21DF
	Home      = 0x21F1
	End       = 0x21F2
	LeftShift = 0x21E7
	LeftCtrl  = 0x2303
	LeftAlt   = 0x2325
	LeftSuper = 0x2318
	F1        = 0xE100
	F2        = 0xE101
	F3        = 0xE102
	F4        = 0xE103
	F5        = 0xE104
	F6        = 0xE105
	F7        = 0xE106
	F8        = 0xE107
	F9        = 0xE108
	F10       = 0xE109
	F11       = 0xE10A
	F12       = 0xE10B
}

pub enum MouseButton u16 {
	Left   = 0
	Right  = 1
	Middle = 2
	X1     = 3
	X2     = 4
}

pub enum EventType u16 {
	Invalid       = 0
	Signal        = 1
	PointerEnter  = 2
	PointerLeave  = 3
	PointerDown   = 4
	PointerUp     = 5
	PointerMove   = 6
	PointerCancel = 7
	Scroll        = 8
	GesturePan    = 9
	GesturePinch  = 10
	GestureRotate = 11
	KeyDown       = 12
	KeyUp         = 13
}

pub enum ThreadFlag u64 {
	NoGui = 1 << 0
	Audio = 1 << 1
}

pub enum ThreadSignal u32 {
	Running    = 1 << 0
	Terminated = 1 << 1
	Writable   = 1 << 2
	Readable   = 1 << 3
}

pub enum WindowSignal u32 {
	Resize    = 1 << 0
	Move      = 1 << 1
	FrameSync = 1 << 2
	Repaint   = 1 << 3
	Key       = 1 << 4
}

pub enum WindowSetRectFlag u64 {
	Outer   = 1 << 0
	Animate = 1 << 1
	Center  = 1 << 2
}

pub enum ClockSignal u32 {
	TimeZone = 1 << 0
}

pub enum ObjectObserveFlag u32 {
	Once = 1 << 0
	Add  = 1 << 1
}

pub enum CallOp u32 {
	ObjectSignal          = 55
	ClockRead             = 25
	ClockMonotonic        = 56
	ObjectObserve         = 57
	EventPoll             = 58
	ThreadEnterMain       = 59
	ClockReadInfo         = 60
	WindowFrameSyncEnable = 63
	WindowInfoGet         = 64
	ThreadLogWrite        = 32
	ThreadStart           = 33
	ThreadExitStatus      = 47
	ThreadWindowCreate    = 41
	ThreadExit            = 30
	ThreadExitProcess     = 31
	HandleClose           = 70
	HandleDuplicate       = 71
	HandleList            = 77
	WindowSetTitle        = 80
	WindowSetRect         = 87
}

pub struct Buf {
	bytes *u8
	len   uint
}

pub struct HandleInfo {
	handle     Handle
	rights     Rights
	name       HandleName
	objectType ObjectType
	objectId   ObjectId
}

pub struct Event {
	size      u16
	eventType u16
	objectId  ObjectId
}

pub struct SignalEvent {
	event        Event
	handle       Handle
	signals      Signals
	pulseSignals Signals
	objectType   ObjectType
	_reserved    u16
}

pub struct InputEvent {
	event     Event
	timestamp Time
	clientId  u32
	deviceId  u32
	modifiers u16
	_reserved u16
}

pub struct PointerEvent {
	inputEvent InputEvent
	pointerId  u32
	flags      u16
	buttons    u16
	button     u8
	kind       PointerKind
	clickCount u8
	_reserved  u8
	x          f32
	y          f32
	dx         f32
	dy         f32
}

pub struct PenPointerEvent {
	pointerEvent       PointerEvent
	pressure           f32
	tangentialPressure f32
	tiltX              f32
	tiltY              f32
	twist              f32
	width              f32
	height             f32
	altitudeAngle      f32
	azimuthAngle       f32
}

pub struct ScrollEvent {
	inputEvent InputEvent
	kind       PointerKind
	phase      ScrollPhase
	flags      u16
	x          f32
	y          f32
	dx         f32
	dy         f32
	wheelZ     f32
}

pub struct GestureEvent {
	inputEvent InputEvent
	phase      GesturePhase
	_reserved  u8
	flags      u16
	x          f32
	y          f32
	dx         f32
	dy         f32
	scale      f32
	rotation   f32
}

pub struct KeyboardEvent {
	inputEvent InputEvent
	keyCode    KeyboardKey
	deviceCode KeyboardKey
	text       [u32 8]
	textLen    u8
	_reserved  u8
	flags      u16
}

pub struct WindowConfig {
	x         f32
	y         f32
	width     f32
	height    f32
	title     *u8
	titleLen  u32
	_reserved [u64 4]
}

pub struct WindowInfo {
	x          f32
	y          f32
	width      f32
	height     f32
	dpScale    f32
	_reserved1 u32
	frameTime  Time
	_reserved2 [u64 4]
}

pub struct ClockInfo {
	timeZoneName    [u8 63]
	timeZoneNameLen u8
}

pub struct ThreadConfig {
	entry                u64
	arg1                 u64
	arg2                 u64
	stack                rawptr
	stackSize            u32
	rights               Rights
	transferHandles      *Handle
	transferHandlesCount u32
}

const HandleInfoSize u32 = 16

const ClockInfoSize u32 = 64

const WindowConfigSize u32 = 56

const WindowInfoSize u32 = 64

const ThreadConfigSize u32 = 48

@wasm_import("pb", "pb_syscall")
fn pb_syscall(handle i32, op u32, arg1, arg2, arg3, arg4, arg5, arg6 u64) i64

@wasm_import("pb", "pb_syscall_async")
fn pb_syscall_async(handle i32, op u32, arg1, arg2, arg3, arg4, arg5, arg6 u64) i64

fn log_write_impl(message &str, flags u32) Err {
	var bytes &[u8] = message
	var data  *u8   = null as *u8
	if len(bytes) > 0 {
		data = &bytes[0]
	}
	var buf Buf
	buf.bytes = data
	buf.len = len(bytes)
	var bufv *Buf = &buf
	var arg1 u64  = bufv as u64
	return pb_syscall(1, op: 32, arg1, arg2: 1 as u64, arg3: flags as u64, arg4: 0 as u64, arg5: 0 as u64, arg6: 0 as u64) as Err
}

pub fn exit(status i32) {
	_ = pb_syscall(1, op: 31, arg1: status as u64, arg2: 0, arg3: 0, arg4: 0, arg5: 0, arg6: 0)
}

pub fn console_log(message &str, flags i32) {
	_ = log_write_impl(message, flags: flags as u32)
}

pub fn panic(message &str, flags i32) {
	_ = log_write_impl(message, flags: flags as u32)
	exit(1)
}

pub fn thread_enter_main(flags u64) {
	_ = pb_syscall(0, op: 59, arg1: flags, arg2: 0, arg3: 0, arg4: 0, arg5: 0, arg6: 0)
}

pub fn thread_exit(thread Handle, status i32) Err {
	return pb_syscall(thread, op: 30, arg1: status as u64, arg2: 0, arg3: 0, arg4: 0, arg5: 0, arg6: 0) as Err
}

pub fn thread_exit_process(thread Handle, status i32) Err {
	return pb_syscall(thread, op: 31, arg1: status as u64, arg2: 0, arg3: 0, arg4: 0, arg5: 0, arg6: 0) as Err
}

pub fn thread_exit_status(thread Handle) i32 {
	return pb_syscall(thread, op: 47, arg1: 0, arg2: 0, arg3: 0, arg4: 0, arg5: 0, arg6: 0) as i32
}

pub fn thread_log_write(thread Handle, message &str, flags u32) Err {
	var bytes &[u8] = message
	var data  *u8   = null as *u8
	if len(bytes) > 0 {
		data = &bytes[0]
	}
	var buf Buf
	buf.bytes = data
	buf.len = len(bytes)
	var bufv *Buf = &buf
	var arg1 u64  = bufv as u64
	return pb_syscall(thread, op: 32, arg1, arg2: 1 as u64, arg3: flags as u64, arg4: 0 as u64, arg5: 0 as u64, arg6: 0 as u64) as Err
}

pub fn handle_close(handle Handle) Err {
	return pb_syscall(handle, op: 70, arg1: 0, arg2: 0, arg3: 0, arg4: 0, arg5: 0, arg6: 0) as Err
}

pub fn handle_duplicate(handle Handle, rights Rights, name HandleName) Handle {
	return pb_syscall(handle, op: 71, arg1: rights as u64, arg2: name as u64, arg3: 0, arg4: 0, arg5: 0, arg6: 0) as Handle
}

pub fn handle_list(handles *HandleInfo, handlesCap u32, flags, predicate u64) i32 {
	var arg1 u64 = handles as u64
	return pb_syscall(0, op: 77, arg1, arg2: handlesCap as u64, arg3: 16 as u64, arg4: flags, arg5: predicate, arg6: 0 as u64) as i32
}

pub fn object_observe(handle Handle, signals Signals, flags u32) Err {
	return pb_syscall(handle, op: 57, arg1: signals as u64, arg2: flags as u64, arg3: 0, arg4: 0, arg5: 0, arg6: 0) as Err
}

pub fn object_signal(handle Handle, disableUser, enableUser, pulseUser Signals) Err {
	return pb_syscall(handle, op: 55, arg1: disableUser as u64, arg2: enableUser as u64, arg3: pulseUser as u64, arg4: 0, arg5: 0, arg6: 0) as Err
}

pub fn clock_monotonic() Time {
	return pb_syscall(0, op: 56, arg1: 0, arg2: 0, arg3: 0, arg4: 0, arg5: 0, arg6: 0) as Time
}

pub fn clock_read(clock Handle) Date {
	return pb_syscall(clock, op: 25, arg1: 0, arg2: 0, arg3: 0, arg4: 0, arg5: 0, arg6: 0) as Date
}

pub fn clock_read_info(clock Handle, info *ClockInfo) Err {
	var arg1 u64 = info as u64
	return pb_syscall(clock, op: 60, arg1, arg2: 64 as u64, arg3: 0 as u64, arg4: 0 as u64, arg5: 0 as u64, arg6: 0 as u64) as Err
}

pub fn event_poll(buffer *[u8], deadline, leeway u64) i32 {
	var ptr *u8 = null as *u8
	if len(buffer) > 0 {
		ptr = &buffer[0]
	}
	var arg1 u64 = ptr as u64
	return pb_syscall_async(0, op: 58, arg1, arg2: len(buffer) as u64, arg3: deadline, arg4: leeway, arg5: 0 as u64, arg6: 0 as u64) as i32
}

pub fn thread_start(parentThread Handle, flags u64, config *ThreadConfig) Handle {
	var arg2 u64 = config as u64
	return pb_syscall(parentThread, op: 33, arg1: flags, arg2, arg3: 48 as u64, arg4: 0 as u64, arg5: 0 as u64, arg6: 0 as u64) as Handle
}

pub fn window_info_get(window Handle, info *WindowInfo) Err {
	var arg1 u64 = info as u64
	return pb_syscall(window, op: 64, arg1, arg2: 64 as u64, arg3: 0 as u64, arg4: 0 as u64, arg5: 0 as u64, arg6: 0 as u64) as Err
}

pub fn window_set_title(window Handle, title &str) Err {
	var bytes &[u8] = title
	var data  *u8   = null as *u8
	if len(bytes) > 0 {
		data = &bytes[0]
	}
	var arg1 u64 = data as u64
	return pb_syscall(window, op: 80, arg1, arg2: len(bytes) as u64, arg3: 0 as u64, arg4: 0 as u64, arg5: 0 as u64, arg6: 0 as u64) as Err
}

pub fn window_set_rect(window Handle, xBits, yBits, widthBits, heightBits u32, flags u64) Err {
	return pb_syscall(window, op: 87, arg1: xBits as u64, arg2: yBits as u64, arg3: widthBits as u64, arg4: heightBits as u64, arg5: flags, arg6: 0) as Err
}

pub fn window_frame_sync_enable(window Handle, flags u64) Err {
	return pb_syscall(window, op: 63, arg1: flags, arg2: 0, arg3: 0, arg4: 0, arg5: 0, arg6: 0) as Err
}

pub fn thread_window_create(thread Handle, config *WindowConfig) Handle {
	var arg1 u64 = config as u64
	return pb_syscall(thread, op: 41, arg1, arg2: 56 as u64, arg3: 0 as u64, arg4: 0 as u64, arg5: 0 as u64, arg6: 0 as u64) as Handle
}

@export("_start")
pub fn _start() {
	_ = pb_syscall(0, op: CallOp.ThreadEnterMain as u32, arg1: 0, arg2: 0, arg3: 0, arg4: 0, arg5: 0, arg6: 0)
	sl_entry_main()
}
