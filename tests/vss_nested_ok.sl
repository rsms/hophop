struct Section {
    entriesLen u8
    entries    [i64 .entriesLen]
}

struct Metadata {
    id       u8
    type     u8
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
    var p *Packet = 0 as *Packet
    var metadata *Metadata = p.metadata
    var sections *Section = p.sections
    var metadataValue *u8 = metadata.value
    var sectionEntries *i64 = sections.entries
    assert metadataValue == 0 as *u8
    assert sectionEntries == 0 as *i64
}
