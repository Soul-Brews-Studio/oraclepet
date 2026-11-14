// retired — SwiftTerm.TerminalView now owns ANSI parsing and rendering.
//
// The previous hand-rolled stateful SGR/CSI parser (ANSIParser) lived here, and
// PTYTextView + TerminalKeys lived in PTYMirror.swift. All three are gone: a real
// VT/xterm emulator (SwiftTerm) does grid layout, full SGR/256/truecolor, cursor
// addressing, and key-event → PTY-byte translation that the partial parser never did.
//
// File kept (not deleted) per "Nothing is Deleted" — see git history for the original.
