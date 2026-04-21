// Verifies VSS nested is accepted.
struct Section {
	entriesLen u8
	entries    [i64 .entriesLen]
}

struct Metadata {
	id       u8
	kind     u8
	valueLen u8
	value    [u8 .valueLen]
}

struct Packet {
	metadataLen u32
	sectionsLen u32
	metadata    [Metadata .metadataLen]
	sections    [Section .sectionsLen]
}

fn main() {
	var p              *Packet   = (null as rawptr) as *Packet
	var metadata       *Metadata = p.metadata
	var sections       *Section  = p.sections
	var metadataValue  *u8       = metadata.value
	var sectionEntries *i64      = sections.entries
	var noValue        *u8       = (null as rawptr) as *u8
	var noEntries      *i64      = (null as rawptr) as *i64
	assert metadataValue == noValue
	assert sectionEntries == noEntries
}
