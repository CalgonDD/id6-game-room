package main

import (
	"bytes"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

type pspUploadFixture struct {
	gamesFixture
	corrupt bool
	calls   *int
}

func (f pspUploadFixture) runReaderDeadline(cmd string, source io.Reader, limit int, _ time.Duration) ([]byte, error) {
	*f.calls++
	data, err := io.ReadAll(source)
	if err != nil {
		return nil, err
	}
	if f.corrupt && len(data) > 0 {
		data[len(data)-1] ^= 1
	}
	return f.gamesFixture.run(cmd, data, limit)
}

func TestPSPStreamingPublishAndSkip(t *testing.T) {
	dir, restore := useTestDrives(t)
	defer restore()
	calls := 0
	f := pspUploadFixture{gamesFixture: gamesFixture{shellFixture: shellFixture{dir: dir}, mountLog: filepath.Join(dir, "mount.log"), readOnly: true}, calls: &calls}
	data := testPSPISO()
	source := filepath.Join(dir, "game.iso")
	if err := os.WriteFile(source, data, 0600); err != nil {
		t.Fatal(err)
	}
	g := ownGame{Game: "PSP", Sub: "yours", Name: "Game.iso", source: source, Size: int64(len(data))}
	tx := testShellPath(filepath.Join(dir, "tx"))
	gameNotes = nil
	if err := putPSPGames(f, tx, []ownGame{g}); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(dir, "mntdata", "ID6Games", "PSP", "yours", "Game.iso")
	if got, err := os.ReadFile(target); err != nil || !bytes.Equal(got, data) {
		t.Fatalf("HDD copy differs: %v", err)
	}
	if calls != 1 {
		t.Fatalf("stream count %d", calls)
	}
	mount, _ := os.ReadFile(f.mountLog)
	if !strings.Contains(string(mount), "-o ro") {
		t.Fatal("HDD was not returned to read-only")
	}
	if err := putPSPGames(f, tx, []ownGame{g}); err != nil {
		t.Fatal(err)
	}
	if calls != 1 {
		t.Fatal("unchanged PSP file uploaded again")
	}
	if _, err := os.Stat(filepath.Join(dir, "mntdata", "ID6Games", "Retro", "yours", "Game.iso")); !os.IsNotExist(err) {
		t.Fatal("PSP leaked into PS1 namespace")
	}
}
func TestPSPStreamingCorruptionNeverPublishes(t *testing.T) {
	dir, restore := useTestDrives(t)
	defer restore()
	calls := 0
	f := pspUploadFixture{gamesFixture: gamesFixture{shellFixture: shellFixture{dir: dir}, mountLog: filepath.Join(dir, "mount.log")}, corrupt: true, calls: &calls}
	data := testPSPISO()
	source := filepath.Join(dir, "game.iso")
	_ = os.WriteFile(source, data, 0600)
	g := ownGame{Game: "PSP", Sub: "yours", Name: "Game.iso", source: source, Size: int64(len(data))}
	if err := putPSPGames(f, testShellPath(filepath.Join(dir, "tx")), []ownGame{g}); err == nil {
		t.Fatal("corrupt upload published")
	}
	if _, err := os.Stat(filepath.Join(dir, "mntdata", "ID6Games", "PSP", "yours", "Game.iso")); !os.IsNotExist(err) {
		t.Fatal("bad file is live")
	}
	if _, err := os.Stat(f.mountLog); !os.IsNotExist(err) {
		t.Fatal("HDD was remounted before readback verification")
	}
	if entries, _ := os.ReadDir(filepath.Join(dir, "mntshare", "ID6Games-staging")); len(entries) != 0 {
		t.Fatal("temporary HDD staging left behind")
	}
}
func TestPSPNestedAssetsStayInsideGameFolder(t *testing.T) {
	dir, restore := useTestDrives(t)
	defer restore()
	f := gamesFixture{shellFixture: shellFixture{dir: dir}, mountLog: filepath.Join(dir, "mount.log"), readOnly: true}
	tx := testShellPath(filepath.Join(dir, "tx"))
	items := []putItem{{name: "PPSSPP/flash0/font/test.pgf", load: func() ([]byte, error) { return []byte("free replacement font"), nil }}}
	if err := putGameFiles(f, tx, "PSP", items); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, "mntdata", "ID6Games", "PSP", "PPSSPP", "flash0", "font", "test.pgf")); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"../escaped", "/absolute", "PPSSPP/../../escaped", "PPSSPP//bad"} {
		script := gameFilesScript("ops", "PSP", []stagedFile{{"stage", name, "hash", 1}})
		if !strings.HasPrefix(string(script), "echo 'Invalid") {
			t.Fatalf("unsafe asset name accepted: %s", name)
		}
	}
}

type pspRemountFailure struct{ pspUploadFixture }

func (f pspRemountFailure) run(command string, input []byte, limit int) ([]byte, error) {
	if command == "/bin/sh -s" && bytes.Contains(input, []byte("trap restore 0")) {
		override := []byte("mount() { case \"$*\" in *\"-o ro\"*) return 1;; *) : > \"$MOUNT_LOG.rw\";; esac; }\ntrap restore 0")
		input = bytes.Replace(input, []byte("trap restore 0"), override, 1)
	}
	return f.gamesFixture.run(command, input, limit)
}
func TestPSPFailedReadOnlyRestoreIsReported(t *testing.T) {
	dir, restore := useTestDrives(t)
	defer restore()
	calls := 0
	f := pspRemountFailure{pspUploadFixture{gamesFixture: gamesFixture{shellFixture: shellFixture{dir: dir}, mountLog: filepath.Join(dir, "mount.log"), readOnly: true}, calls: &calls}}
	data := testPSPISO()
	source := filepath.Join(dir, "game.iso")
	_ = os.WriteFile(source, data, 0600)
	g := ownGame{Game: "PSP", Sub: "yours", Name: "Game.iso", source: source, Size: int64(len(data))}
	err := putPSPGames(f, testShellPath(filepath.Join(dir, "tx")), []ownGame{g})
	if err == nil || !strings.Contains(err.Error(), "read-only") {
		t.Fatalf("remount failure hidden: %v", err)
	}
}
