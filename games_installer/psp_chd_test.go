package main

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

func TestOwnerCHDGeometryRouting(t *testing.T) {
	if os.Getenv("ID6_PSP_CHD_SAMPLE") == "" || os.Getenv("ID6_PS1_CHD_SAMPLE") == "" {
		t.Skip("optional private owner fixtures")
	}
	for _, tc := range []struct{ path, console string }{
		{os.Getenv("ID6_PSP_CHD_SAMPLE"), "PSP"},
		{os.Getenv("ID6_PS1_CHD_SAMPLE"), "Retro"},
	} {
		f, err := os.Open(tc.path)
		if err != nil {
			t.Fatal(err)
		}
		st, err := f.Stat()
		if err != nil {
			f.Close()
			t.Fatal(err)
		}
		g := ownGame{}
		classifyOwnGame(&g, filepath.Base(tc.path), f, st.Size())
		f.Close()
		if g.Problem != "" || g.Game != tc.console {
			t.Fatalf("%s: %+v", filepath.Base(tc.path), g)
		}
		t.Logf("%s -> %s/%s", filepath.Base(tc.path), g.Game, g.Sub)
	}
}

func testCHD(unit uint32) []byte {
	b := make([]byte, 4096)
	copy(b, "MComprHD")
	binary.BigEndian.PutUint32(b[8:12], 124)
	binary.BigEndian.PutUint32(b[12:16], 5)
	binary.BigEndian.PutUint64(b[32:40], uint64(unit)*36)
	binary.BigEndian.PutUint64(b[40:48], 124)
	binary.BigEndian.PutUint32(b[56:60], unit)
	binary.BigEndian.PutUint32(b[60:64], unit)
	return b
}

func TestCHDConsoleRouting(t *testing.T) {
	for _, tc := range []struct {
		name    string
		unit    uint32
		console string
	}{
		{"GTA Liberty City Stories.chd", 2048, "PSP"},
		{"Tomb Raider.chd", 2448, "Retro"},
	} {
		b := testCHD(tc.unit)
		g := ownGame{}
		classifyOwnGame(&g, tc.name, bytes.NewReader(b), int64(len(b)))
		if g.Problem != "" || g.Game != tc.console || g.Sub != "yours" || g.Name != tc.name {
			t.Fatalf("%s: %+v", tc.name, g)
		}
	}
}

func TestPSPCHDRejectInvalidGeometryAndParent(t *testing.T) {
	for _, mutate := range []func([]byte){
		func(b []byte) { b[104] = 1 },
		func(b []byte) { binary.BigEndian.PutUint64(b[40:48], uint64(len(b))) },
		func(b []byte) { binary.BigEndian.PutUint64(b[48:56], uint64(len(b)-1)) },
		func(b []byte) { binary.BigEndian.PutUint64(b[32:40], 1<<63) },
		func(b []byte) { binary.BigEndian.PutUint32(b[56:60], 0) },
		func(b []byte) { binary.BigEndian.PutUint32(b[8:12], 108) },
	} {
		b := testCHD(2048)
		mutate(b)
		g := ownGame{}
		classifyOwnGame(&g, "bad.chd", bytes.NewReader(b), int64(len(b)))
		if g.Problem == "" || g.Game == "Retro" {
			t.Fatalf("invalid DVD CHD escaped validation: %+v", g)
		}
	}
	b := testCHD(2448)
	if validatePSPCHD(bytes.NewReader(b), int64(len(b))) == nil {
		t.Fatal("CD image accepted as PSP")
	}
	if validatePSPCHD(bytes.NewReader(b[:64]), 64) == nil {
		t.Fatal("truncated header accepted")
	}
}
