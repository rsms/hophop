// Verifies string literals is accepted.
fn main() {
	assert "Hello, world!\n" == "Hello, world!\x0a"
	assert "\\n" == "\n"
	assert "\\\"" == "\""
	assert "日本語" == "\u65e5\u672c\u8a9e"
	assert "\u65e5本\U00008a9e" == "日本語"

	assert "A cool cat 😎" == "A cool cat \U0001F60E"
	assert "A cool cat 😎" == "A cool cat \xF0\x9F\x98\x8E"
	assert "Square root: √" == "Square root: \u221A"
	assert "Lycian An: 𐊙" == "Lycian An: \U00010299"

	assert "日本語" == "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"
	assert "👩🏽‍🚀" == "\xf0\x9f\x91\xa9\xf0\x9f\x8f\xbd\xe2\x80\x8d\xf0\x9f\x9a\x80"

	assert `abc` == "abc"

	var raw = `this
    is a "raw" stri\ng literal with a \` backtick`
	assert raw == "this\n    is a \"raw\" stri\\ng literal with a ` backtick"

	assert "hello \
world" == "hello world"
	assert "hello 
world" == "hello \nworld"

	var message = "hel" + "lo " + "wor" + "ld"
	assert message == "hello world"

	var m *str = "mutable " + "string"
	assert len(m) == 14
}
