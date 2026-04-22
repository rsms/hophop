// string literal features: UTF-8, escapes, raw strings, multiline forms, and literal concatenation
fn main() {
	// verbatim and escaped Unicode both decode to the same UTF-8 bytes
	assert "日本語" == "\u65e5\u672c\u8a9e"
	assert "日本語" == "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"
	assert "A cool cat 😎" == "A cool cat \U0001F60E"

	// interpreted strings can continue across a source line break with backslash-newline
	assert "hello \
world" == "hello world"

	// verbatim line breaks are preserved as '\n'
	assert "hello
world" == "hello\nworld"

	// raw strings keep bytes verbatim except that \` encodes a literal backtick
	var raw = `this
    is a "raw" stri\ng literal`
	assert raw == "this\n    is a \"raw\" stri\\ng literal"
	assert `\`` == "`"

	// literal-only concatenation is folded at compile time
	var message = "hel" + "lo " + "wor" + "ld"
	assert message == "hello world"
}
