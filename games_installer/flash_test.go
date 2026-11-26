package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestFlashReport(t *testing.T) {
	// the car's own df line for /var (pull 2026-11-20 13:30): 492 MiB, ~342 MiB free
	total, free, ok := parseDf("/dev/sda0t178             1007552    307456    700096      31%  /fs/sda1/       \n")
	if !ok || total < 491 || total > 493 || free < 341 || free > 343 {
		t.Fatalf("df: %v %v %v", total, free, ok)
	}
	mb, n := parseDu("2048\t/var/opt/hmi/data/ID5/Retro-data/a.state\n10240\t/var/x\n")
	if n != 2 || mb != 6 {
		t.Fatalf("du: %v %v", mb, n)
	}
	if line := flashLine(flashInfo{ok: true, totalMB: 492, freeMB: 300}); strings.Contains(line, "WARNING") || !strings.Contains(line, "300 MB free of 492 MB") {
		t.Fatalf("plenty: %q", line)
	}
	if line := flashLine(flashInfo{ok: true, totalMB: 492, freeMB: 120}); !strings.Contains(line, "WARNING") {
		t.Fatalf("low: %q", line)
	}
}

func TestDeleteSaveStatesKeepsEverythingElse(t *testing.T) {
	dir := t.TempDir()
	write := func(rel string) string {
		p := filepath.Join(dir, rel)
		if err := os.MkdirAll(filepath.Dir(p), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte("x"), 0644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	gone := []string{write("Retro-data/Crash Bandicoot.state"), write("Retro-data/uCity.state"), write("Retro-data/x.state.new")}
	kept := []string{write("Retro-data/PS1 memory card.srm"), write("Retro-data/uCity.srm"), write("Freedoom-data/.savegame/doomsav0.dsg"), write("LibreQuake-data/id1/s0.sav")}
	n, err := deleteSaveStates(shellFixture{dir: dir})
	if err != nil || n != 3 {
		t.Fatalf("deleted %d, %v", n, err)
	}
	for _, p := range gone {
		if _, err := os.Stat(p); !os.IsNotExist(err) {
			t.Fatalf("%s is still there", p)
		}
	}
	for _, p := range kept {
		if _, err := os.Stat(p); err != nil {
			t.Fatalf("%s was removed", p)
		}
	}
}

// The production firmware df listing puts RAM/HDD before the eMMC /var mount.
// A failed df of the ID5 subdirectory must not report those as flash.
type fallbackFlashRunner struct{ all string }

func (r fallbackFlashRunner) run(cmd string, _ []byte, _ int) ([]byte, error) {
	if cmd == "df "+root+" 2>/dev/null" {
		return nil, fmt.Errorf("subdirectory df unsupported")
	}
	if cmd == "df 2>/dev/null" {
		return []byte(r.all), nil
	}
	return []byte("states\n"), nil
}
func TestFlashSpaceFallbackSelectsVarNotHDDOrRAM(t *testing.T) {
	prefix := "/dev/ram 88064 1464 86600 2% /ramdisk/mm/\n/dev/hd0t177 277192672 88461792 188730880 32% /mnt/data/\n"
	info := flashStatus(fallbackFlashRunner{prefix + "/dev/sda0t178 1007552 180392 827160 18% /fs/sda1/\n"})
	if !info.ok || info.totalMB < 491 || info.totalMB > 493 || info.freeMB < 403 || info.freeMB > 405 {
		t.Fatalf("wrong flash selection: %+v", info)
	}
	info = flashStatus(fallbackFlashRunner{prefix})
	if info.ok {
		t.Fatalf("reported another volume as flash: %+v", info)
	}
	if !strings.Contains(flashLine(info), "does not mean") {
		t.Fatal("unknown capacity must not be reported as full")
	}
}
