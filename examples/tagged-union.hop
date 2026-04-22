// tagged enums (tagged unions):
// - payload and non-payload variants
// - payload field defaults (omitted fields zero-initialize)
// - switch-only narrowing and `as` aliases
// - comparisons (== uses tag+payload, ordering uses tag)
enum Event u8 {
	Idle = 0
	Click{
		x i32
		y i32
	}
	Key{
		code   i32
		repeat bool
	} = 8
	Resize{
		width  i32
		height i32
	}
}

fn score(e Event) i32 {
	switch e {
		case Event.Idle  { return 0 }
		case Event.Click { return e.x + e.y }
		case Event.Key as k {
			if k.repeat {
				return -k.code
			}
			return k.code
		}
		case Event.Resize as r { return r.width * r.height }
	}
}

fn from_call() Event {
	return Event.Click{ x: 4, y: 5 }
}

fn score_from_call() i32 {
	// When switching over a non-identifier expression, use `as` to access payload.
	switch from_call() {
		case Event.Idle        { return 0 }
		case Event.Click as c  { return c.x * c.y }
		case Event.Key as k    { return k.code }
		case Event.Resize as r { return r.width + r.height }
	}
}

fn main() {
	var a Event = Event.Resize{ width: 10 }            // height defaults to 0
	var b Event = Event.Resize{ width: 10, height: 0 } // same value as `a`

	assert a == b
	assert Event.Idle < Event.Click{ x: 1, y: 2 }

	assert score(Event.Click{ x: 2, y: 3 }) == 5
	assert score(Event.Key{ code: 7, repeat: true }) == -7
	assert score_from_call() == 20
}
