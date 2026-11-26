package main

import (
	"bytes"
	"compress/flate"
	"encoding/binary"
	"io"
	"os"
	"path/filepath"
	"testing"
)

func testPSPISO() []byte {
	disc := make([]byte, 36*2048)
	p := disc[16*2048 : 17*2048]
	p[0] = 1
	copy(p[1:], "CD001")
	p[6] = 1
	binary.LittleEndian.PutUint16(p[128:], 2048)
	binary.BigEndian.PutUint16(p[130:], 2048)
	r := p[156:190]
	r[0] = 34
	r[25] = 2
	r[32] = 1
	binary.LittleEndian.PutUint32(r[2:], 20)
	binary.BigEndian.PutUint32(r[6:], 20)
	binary.LittleEndian.PutUint32(r[10:], 2048)
	binary.BigEndian.PutUint32(r[14:], 2048)
	record := disc[20*2048 : 21*2048]
	record[0] = 42
	record[25] = 2
	record[32] = 8
	copy(record[33:], "PSP_GAME")
	return disc
}
func testPSPCSO(iso []byte, compressed bool) []byte {
	blocks := len(iso) / 2048
	out := make([]byte, 24+(blocks+1)*4)
	copy(out, "CISO")
	binary.LittleEndian.PutUint32(out[4:], 24)
	binary.LittleEndian.PutUint64(out[8:], uint64(len(iso)))
	binary.LittleEndian.PutUint32(out[16:], 2048)
	out[20] = 1
	for i := 0; i < blocks; i++ {
		offset := uint32(len(out))
		if !compressed {
			offset |= 0x80000000
		}
		binary.LittleEndian.PutUint32(out[24+i*4:], offset)
		if compressed {
			var b bytes.Buffer
			w, _ := flate.NewWriter(&b, flate.BestSpeed)
			_, _ = w.Write(iso[i*2048 : (i+1)*2048])
			_ = w.Close()
			out = append(out, b.Bytes()...)
		} else {
			out = append(out, iso[i*2048:(i+1)*2048]...)
		}
	}
	binary.LittleEndian.PutUint32(out[24+blocks*4:], uint32(len(out)))
	return out
}
func testPSPPBP() []byte {
	b := make([]byte, 92)
	copy(b, "\x00PBP")
	binary.LittleEndian.PutUint32(b[4:], 0x10000)
	for i := 0; i < 8; i++ {
		off := uint32(40)
		if i == 7 {
			off = 92
		}
		binary.LittleEndian.PutUint32(b[8+i*4:], off)
	}
	copy(b[40:], "\x7fELF")
	b[44] = 1
	b[45] = 1
	binary.LittleEndian.PutUint16(b[58:], 8)
	return b
}
func TestPSPClassificationAndNamespace(t *testing.T) {
	iso := testPSPISO()
	cases := []struct {
		name string
		data []byte
	}{{"Test.iso", iso}, {"Test.cso", testPSPCSO(iso, false)}, {"Compressed.CSO", testPSPCSO(iso, true)}, {"EBOOT.pbp", testPSPPBP()}}
	for _, c := range cases {
		g := classified(c.name, c.data)
		if g.Problem != "" || g.Game != "PSP" || g.Sub != "yours" {
			t.Fatalf("%s: %+v", c.name, g)
		}
	}
	bad := append([]byte(nil), iso...)
	copy(bad[20*2048+33:], "PS1_GAME")
	if g := classified("PS1.iso", bad); g.Problem == "" {
		t.Fatal("non-PSP ISO accepted")
	}
	if g := classified("fake.pbp", bytes.Repeat([]byte{0}, 100)); g.Problem == "" {
		t.Fatal("extension-only PBP accepted")
	}
	classic := append(testPSPPBP(), []byte("PSISOIMG0000")...)
	if g := classified("Classic.pbp", classic); g.Problem == "" {
		t.Fatal("PS1 Classics PBP accepted as PSP")
	}
	if !ownGamePathOK("PSP/yours/Game.iso") || ownGamePathOK("PSP/PPSSPP/compat.ini") {
		t.Fatal("owner removal namespace is wrong")
	}
}
func TestPSPRejectsMalformedRanges(t *testing.T) {
	iso := testPSPISO()
	for _, mutate := range []func([]byte){
		func(b []byte) { b[156+16*2048] = 0 },
		func(b []byte) { binary.LittleEndian.PutUint32(b[16*2048+166:], 2<<20) },
		func(b []byte) { b[20*2048] = 33 },
	} {
		bad := append([]byte(nil), iso...)
		mutate(bad)
		if g := classified("bad.iso", bad); g.Problem == "" {
			t.Fatal("malformed ISO accepted")
		}
	}
	cso := testPSPCSO(iso, true)
	for _, mutate := range []func([]byte){
		func(b []byte) { b[20] = 2 }, func(b []byte) { b[21] = 31 },
		func(b []byte) { binary.LittleEndian.PutUint64(b[8:], ^uint64(0)) },
		func(b []byte) { binary.LittleEndian.PutUint32(b[24+16*4:], 1) },
		func(b []byte) { binary.LittleEndian.PutUint32(b[len(iso)/2048*4+24:], 0x7fffffff) },
	} {
		bad := append([]byte(nil), cso...)
		mutate(bad)
		if g := classified("bad.cso", bad); g.Problem == "" {
			t.Fatal("malformed CISO accepted")
		}
	}
	pbp := testPSPPBP()
	binary.LittleEndian.PutUint32(pbp[36:], 41)
	if g := classified("bad.pbp", pbp); g.Problem == "" {
		t.Fatal("overlapping executable section accepted")
	}
}

type countingPSPReader struct {
	r     io.ReaderAt
	bytes int
	limit int
}

func (r *countingPSPReader) ReadAt(b []byte, o int64) (int, error) {
	r.bytes += len(b)
	if r.bytes > r.limit {
		panic("unbounded classifier read")
	}
	return r.r.ReadAt(b, o)
}
func TestPSPClassificationReadsBoundedSectors(t *testing.T) {
	iso := testPSPISO()
	r := &countingPSPReader{r: bytes.NewReader(iso), limit: 8192}
	d, err := newPSPDisc(r, 1800<<20, false)
	if err != nil {
		t.Fatal(err)
	}
	if err := validatePSPISO(d); err != nil {
		t.Fatal(err)
	}
	if r.bytes > 4096 {
		t.Fatalf("read %d bytes", r.bytes)
	}
}

func TestPSPUpstreamSamples(t *testing.T) {
	dir := os.Getenv("ID6_PSP_TEST_SAMPLES")
	if dir == "" {
		t.Skip("optional pinned upstream PSP samples not supplied")
	}
	for _, name := range []string{"demos/cube.iso", "demos/cube.pbp", "input/cube.cso"} {
		path := filepath.Join(dir, filepath.FromSlash(name))
		st, err := os.Stat(path)
		if err != nil {
			t.Fatal(err)
		}
		games := checkOwnFile(path, st.Size())
		if len(games) != 1 || games[0].Problem != "" || games[0].Game != "PSP" {
			t.Fatalf("%s: %+v", name, games)
		}
	}
}
