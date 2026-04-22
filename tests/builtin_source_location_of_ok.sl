// Verifies builtin source_location_of is accepted.
import "builtin"

const HERE builtin.SourceLocation = builtin.source_location_of(HERE)

const UNQUALIFIED builtin.SourceLocation = source_location_of(UNQUALIFIED)
