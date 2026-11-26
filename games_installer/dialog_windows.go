//go:build windows

package main

// The Windows file and folder pickers for "Your games" (comdlg32 / shell32, no extra dependencies).

import (
	"path/filepath"
	"unsafe"

	"golang.org/x/sys/windows"
)

var (
	comdlg32                = windows.NewLazySystemDLL("comdlg32.dll")
	procGetOpenFileNameW    = comdlg32.NewProc("GetOpenFileNameW")
	shell32                 = windows.NewLazySystemDLL("shell32.dll")
	procSHBrowseForFolder   = shell32.NewProc("SHBrowseForFolderW")
	procSHGetPathFromIDList = shell32.NewProc("SHGetPathFromIDListW")
	ole32                   = windows.NewLazySystemDLL("ole32.dll")
	procCoTaskMemFree       = ole32.NewProc("CoTaskMemFree")
)

type openFileName struct {
	structSize    uint32
	owner         uintptr
	instance      uintptr
	filter        *uint16
	customFilter  *uint16
	maxCustFilter uint32
	filterIndex   uint32
	file          *uint16
	maxFile       uint32
	fileTitle     *uint16
	maxFileTitle  uint32
	initialDir    *uint16
	title         *uint16
	flags         uint32
	fileOffset    uint16
	fileExtension uint16
	defExt        *uint16
	custData      uintptr
	hook          uintptr
	templateName  *uint16
	reserved      uintptr
	reservedDword uint32
	flagsEx       uint32
}

type browseInfo struct {
	owner       uintptr
	root        uintptr
	displayName *uint16
	title       *uint16
	flags       uint32
	callback    uintptr
	param       uintptr
	image       int32
}

// utf16z turns "a\x00b\x00\x00"-style strings into a UTF-16 buffer, keeping the embedded zeros.
func utf16z(s string) []uint16 {
	out := make([]uint16, 0, len(s)+1)
	for _, r := range s {
		out = append(out, uint16(r))
	}
	return append(out, 0)
}

// pickFiles shows the open-file dialog (several files allowed); nil when canceled.
func pickFiles(owner unsafe.Pointer, title string) []string {
	buf := make([]uint16, 1<<16)
	filter := utf16z("Game files (.nes .gb .gbc .chd .iso .cso .pbp .wad .pak .zip, PS1 BIOS .bin)\x00*.nes;*.gb;*.gbc;*.chd;*.iso;*.cso;*.pbp;*.wad;*.pak;*.zip;*.bin\x00All files\x00*.*\x00")
	t, _ := windows.UTF16PtrFromString(title)
	ofn := openFileName{
		owner:       uintptr(owner),
		filter:      &filter[0],
		filterIndex: 1,
		file:        &buf[0],
		maxFile:     uint32(len(buf)),
		title:       t,
		flags:       0x00080000 | 0x00000200 | 0x00001000 | 0x00000800 | 0x00000008 | 0x00000004, // explorer, multi, file+path must exist, no chdir, hide read-only
	}
	ofn.structSize = uint32(unsafe.Sizeof(ofn))
	if ok, _, _ := procGetOpenFileNameW.Call(uintptr(unsafe.Pointer(&ofn))); ok == 0 {
		return nil
	}
	var parts []string
	start := 0
	for i, c := range buf {
		if c != 0 {
			continue
		}
		if i == start {
			break
		}
		parts = append(parts, windows.UTF16ToString(buf[start:i]))
		start = i + 1
	}
	if len(parts) <= 1 {
		return parts
	}
	var paths []string
	for _, name := range parts[1:] {
		paths = append(paths, filepath.Join(parts[0], name))
	}
	return paths
}

// pickFolder shows the folder picker; "" when canceled.
func pickFolder(owner unsafe.Pointer, title string) string {
	name := make([]uint16, 1024)
	t, _ := windows.UTF16PtrFromString(title)
	bi := browseInfo{owner: uintptr(owner), displayName: &name[0], title: t, flags: 0x1 | 0x40 | 0x200} // file-system folders, new style, no "new folder"
	pidl, _, _ := procSHBrowseForFolder.Call(uintptr(unsafe.Pointer(&bi)))
	if pidl == 0 {
		return ""
	}
	defer procCoTaskMemFree.Call(pidl)
	path := make([]uint16, 1024)
	if ok, _, _ := procSHGetPathFromIDList.Call(pidl, uintptr(unsafe.Pointer(&path[0]))); ok == 0 {
		return ""
	}
	return windows.UTF16ToString(path)
}
