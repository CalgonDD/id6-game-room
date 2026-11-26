//go:build !windows

package main

import "unsafe"

// The pickers only exist on Windows (the installer's platform); elsewhere nothing is chosen.
func pickFiles(owner unsafe.Pointer, title string) []string { return nil }
func pickFolder(owner unsafe.Pointer, title string) string  { return "" }
