package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
)

// CHD v5 records its logical sector size in the header. PSP DVD-mode images
// use 2048-byte units; PS1 CD images use 2448-byte units. This inspects only
// the container, not decompressed PSP_GAME content or codec compatibility.
func validatePSPCHD(r io.ReaderAt, size int64) error {
	h := pspReadAt(r, 0, 124, size)
	if len(h) != 124 || string(h[:8]) != "MComprHD" || binary.BigEndian.Uint32(h[8:12]) != 124 || binary.BigEndian.Uint32(h[12:16]) != 5 {
		return errors.New("PSP CHD requires a version 5 DVD-mode image")
	}
	logical := binary.BigEndian.Uint64(h[32:40])
	mapOffset := binary.BigEndian.Uint64(h[40:48])
	metaOffset := binary.BigEndian.Uint64(h[48:56])
	hunk := binary.BigEndian.Uint32(h[56:60])
	unit := binary.BigEndian.Uint32(h[60:64])
	if size > ownMaxDisc || unit != 2048 || logical < 17*2048 || logical > ownMaxDisc || logical%2048 != 0 ||
		hunk < 2048 || hunk > 512*1024 || hunk%2048 != 0 || mapOffset < 124 || mapOffset >= uint64(size) ||
		(metaOffset != 0 && (metaOffset < 124 || metaOffset > uint64(size)-16)) {
		return errors.New("invalid PSP CHD header, size or sector geometry; use DVD mode with 2048-byte sectors")
	}
	if !bytes.Equal(h[104:124], make([]byte, 20)) {
		return errors.New("parent-dependent PSP CHDs are unsupported; use a self-contained image")
	}
	return nil
}

func isPSPCHD(r io.ReaderAt, size int64) bool {
	h := pspReadAt(r, 0, 64, size)
	return len(h) == 64 && string(h[:8]) == "MComprHD" &&
		binary.BigEndian.Uint32(h[12:16]) == 5 && binary.BigEndian.Uint32(h[60:64]) == 2048
}
