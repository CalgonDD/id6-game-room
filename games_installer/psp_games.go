package main

// Bounded structural checks, not an emulator compatibility guarantee. Disc
// sectors are read on demand; a 1.8 GB ISO is never loaded to classify it.
import (
	"bytes"
	"compress/flate"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

type pspDisc struct {
	r                 io.ReaderAt
	physical, logical int64
	cso               bool
	header            int64
	align             uint
}

func pspReadAt(r io.ReaderAt, off int64, n int, size int64) []byte {
	if off < 0 || n < 0 || off > size || int64(n) > size-off {
		return nil
	}
	b := make([]byte, n)
	got, err := r.ReadAt(b, off)
	if got != n || (err != nil && err != io.EOF) {
		return nil
	}
	return b
}

func newPSPDisc(r io.ReaderAt, size int64, cso bool) (*pspDisc, error) {
	d := &pspDisc{r: r, physical: size, logical: size, cso: cso}
	if size < 24 || (!cso && size < 17*2048) || size > ownMaxDisc {
		return nil, errors.New("disc size is outside the supported PSP range")
	}
	if !cso {
		return d, nil
	}
	h := pspReadAt(r, 0, 24, size)
	if len(h) != 24 || string(h[:4]) != "CISO" || h[20] != 1 || h[21] > 15 || binary.LittleEndian.Uint32(h[16:20]) != 2048 {
		return nil, errors.New("not a supported CISO version 1 disc with 2048-byte sectors")
	}
	d.header = int64(binary.LittleEndian.Uint32(h[4:8]))
	// Many version-1 tools leave header_size zero. PPSSPP locates the v1
	// index immediately after the fixed 24-byte header in either case.
	if d.header == 0 {
		d.header = 24
	}
	d.logical = int64(binary.LittleEndian.Uint64(h[8:16]))
	d.align = uint(h[21])
	if d.header != 24 || d.logical < 17*2048 || d.logical > ownMaxDisc || d.logical%2048 != 0 {
		return nil, errors.New("invalid CISO header or expanded size")
	}
	blocks := d.logical / 2048
	if d.header+(blocks+1)*4 > size {
		return nil, errors.New("truncated CISO index")
	}
	first := pspReadAt(r, d.header, 4, size)
	last := pspReadAt(r, d.header+blocks*4, 4, size)
	start := int64(binary.LittleEndian.Uint32(first)&0x7fffffff) << d.align
	end := int64(binary.LittleEndian.Uint32(last)&0x7fffffff) << d.align
	if start < d.header+(blocks+1)*4 || end < start || end > size {
		return nil, errors.New("CISO index points outside the file")
	}
	return d, nil
}

func (d *pspDisc) sector(number uint32) ([]byte, error) {
	offset := int64(number) * 2048
	if offset < 0 || offset+2048 > d.logical {
		return nil, errors.New("disc sector is out of range")
	}
	if !d.cso {
		b := pspReadAt(d.r, offset, 2048, d.physical)
		if len(b) != 2048 {
			return nil, io.ErrUnexpectedEOF
		}
		return b, nil
	}
	indices := pspReadAt(d.r, d.header+int64(number)*4, 8, d.physical)
	if len(indices) != 8 {
		return nil, io.ErrUnexpectedEOF
	}
	a, b := binary.LittleEndian.Uint32(indices), binary.LittleEndian.Uint32(indices[4:])
	start, end := int64(a&0x7fffffff)<<d.align, int64(b&0x7fffffff)<<d.align
	tableEnd := d.header + (d.logical/2048+1)*4
	if start < tableEnd || end <= start || end > d.physical || end-start > 128<<10 {
		return nil, errors.New("invalid CISO sector range")
	}
	if a&0x80000000 != 0 {
		if end-start < 2048 {
			return nil, io.ErrUnexpectedEOF
		}
		out := pspReadAt(d.r, start, 2048, d.physical)
		if len(out) != 2048 {
			return nil, io.ErrUnexpectedEOF
		}
		return out, nil
	}
	compressed := pspReadAt(d.r, start, int(end-start), d.physical)
	reader := flate.NewReader(bytes.NewReader(compressed))
	defer reader.Close()
	out := make([]byte, 2048)
	if _, err := io.ReadFull(reader, out); err != nil {
		return nil, fmt.Errorf("CISO sector cannot be decompressed: %w", err)
	}
	var extra [1]byte
	if n, err := reader.Read(extra[:]); n != 0 || err != io.EOF {
		return nil, errors.New("CISO sector expands past 2048 bytes")
	}
	return out, nil
}

func validatePSPISO(d *pspDisc) error {
	var pvd []byte
	for sector := uint32(16); sector < 32; sector++ {
		b, err := d.sector(sector)
		if err != nil {
			return err
		}
		if string(b[1:6]) != "CD001" || b[6] != 1 {
			return errors.New("missing ISO9660 volume descriptor")
		}
		if b[0] == 1 {
			pvd = b
			break
		}
		if b[0] == 255 {
			break
		}
	}
	if pvd == nil || binary.LittleEndian.Uint16(pvd[128:130]) != 2048 || binary.BigEndian.Uint16(pvd[130:132]) != 2048 {
		return errors.New("missing PSP ISO primary volume or sector size")
	}
	root := pvd[156:190]
	extent, length := binary.LittleEndian.Uint32(root[2:6]), binary.LittleEndian.Uint32(root[10:14])
	if root[0] < 34 || root[25]&2 == 0 || extent != binary.BigEndian.Uint32(root[6:10]) || length != binary.BigEndian.Uint32(root[14:18]) ||
		length == 0 || length > 1<<20 || int64(extent)*2048+int64(length) > d.logical {
		return errors.New("invalid ISO root directory")
	}
	found := false
	for pos := uint32(0); pos < length; pos += 2048 {
		data, err := d.sector(extent + pos/2048)
		if err != nil {
			return err
		}
		remaining := int(length - pos)
		if remaining < len(data) {
			data = data[:remaining]
		}
		for off := 0; off < len(data) && data[off] != 0; {
			n := int(data[off])
			if n < 34 || off+n > len(data) {
				return errors.New("invalid ISO directory record")
			}
			record := data[off : off+n]
			nameLen := int(record[32])
			if 33+nameLen > len(record) {
				return errors.New("invalid ISO filename")
			}
			if string(record[33:33+nameLen]) == "PSP_GAME" && record[25]&2 != 0 {
				found = true
			}
			off += n
		}
	}
	if !found {
		return errors.New("ISO does not contain a PSP_GAME directory; PS1 images belong in Retro as CHD")
	}
	return nil
}

func validatePSPPBP(r io.ReaderAt, size int64) error {
	h := pspReadAt(r, 0, 40, size)
	if len(h) != 40 || string(h[:4]) != "\x00PBP" {
		return errors.New("missing PBP header")
	}
	offsets := make([]int64, 9)
	offsets[8] = size
	for i := 0; i < 8; i++ {
		offsets[i] = int64(binary.LittleEndian.Uint32(h[8+i*4:]))
		if offsets[i] < 40 || offsets[i] > size || (i > 0 && offsets[i] < offsets[i-1]) {
			return errors.New("PBP sections are out of range or overlap")
		}
	}
	psar := pspReadAt(r, offsets[7], int(min(int64(16), size-offsets[7])), size)
	if bytes.HasPrefix(psar, []byte("PSISOIMG")) || bytes.HasPrefix(psar, []byte("PSTITLEIMG")) {
		return errors.New("PS1 Classics EBOOT is not a PSP game; it cannot be added to the PSP folder")
	}
	if offsets[7]-offsets[6] < 52 {
		return errors.New("PBP has no PSP executable section")
	}
	code := pspReadAt(r, offsets[6], 52, size)
	if bytes.HasPrefix(code, []byte("~PSP")) {
		if offsets[7]-offsets[6] < 0x150 {
			return errors.New("truncated encrypted PSP executable")
		}
		return nil
	}
	if len(code) != 52 || string(code[:4]) != "\x7fELF" || code[4] != 1 || code[5] != 1 || binary.LittleEndian.Uint16(code[18:20]) != 8 {
		return errors.New("PBP executable is not a PSP MIPS ELF or ~PSP image")
	}
	return nil
}

func classifyPSP(g *ownGame, base, ext string, r io.ReaderAt, size int64) {
	var err error
	switch ext {
	case ".chd":
		err = validatePSPCHD(r, size)
	case ".iso", ".cso":
		var disc *pspDisc
		disc, err = newPSPDisc(r, size, ext == ".cso")
		if err == nil {
			err = validatePSPISO(disc)
		}
	case ".pbp":
		err = validatePSPPBP(r, size)
	default:
		err = errors.New("unsupported PSP format")
	}
	if err != nil {
		g.Problem = "PSP: " + err.Error()
		return
	}
	g.Game, g.Sub, g.Name, g.Title = "PSP", "yours", carFileName(base)+ext, "PSP: "+carFileName(base)
	g.Note = "Stored on the hard drive. PSP saves use the separate PSP-data folder. Gameplay performance needs testing on iDrive."
	if ext == ".chd" {
		g.Note += " DVD-mode CHD: container checked; PSP game contents and compression are checked by the emulator at launch. Other consoles' DVD images are not PSP games. 2048-byte hunks are recommended for speed."
	}
}
