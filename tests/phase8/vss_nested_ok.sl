struct Section {
    entriesLen u8
    entries    [.entriesLen]i64
}

struct Metadata {
    id       u8
    type     u8
    valueLen u8
    value    [.valueLen]u8
}

struct Packet {
    metadataLen u32
    sectionsLen u32
    metadata    [.metadataLen]Metadata
    sections    [.sectionsLen]Section
}

fn main() i32 {
    var p *Packet = 0 as *Packet
    var metadata *Metadata = p.metadata
    var sections *Section = p.sections
    var metadataValue *u8 = metadata.value
    var sectionEntries *i64 = sections.entries
    return (metadataValue != 0 as *u8 || sectionEntries != 0 as *i64) as i32
}
