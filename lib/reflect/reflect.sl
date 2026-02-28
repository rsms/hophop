pub enum Kind u8 {
	Invalid   = 0
	Primitive = 1
	Alias     = 2
	Struct    = 3
	Union     = 4
	Enum      = 5
	Pointer   = 6
	Reference = 7
	Slice     = 8
	Array     = 9
	Optional  = 10
	Function  = 11
	Tuple     = 12
}
