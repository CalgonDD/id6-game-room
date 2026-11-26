package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// gamesFixture points the hard drive and /mnt/share at temporary folders. readOnly makes the hard drive report
// itself read-only until the script remounts it (a fake mount logs every call).
type gamesFixture struct {
	shellFixture
	mountLog string
	readOnly bool
}

func (f gamesFixture) run(command string, input []byte, limit int) ([]byte, error) {
	if command == "/bin/sh -s" {
		prelude := "MOUNT_LOG=" + testShellPath(f.mountLog) + "\n" +
			`mount() { echo "mount $*" >> "$MOUNT_LOG"; case "$*" in *"-o rw"*) : > "$MOUNT_LOG.rw";; *"-o ro"*) rm -f "$MOUNT_LOG.rw";; esac; }` + "\n"
		if f.readOnly {
			prelude += "FAKE_RO=" + gamesMount + "\n" +
				`test() { if [ "$1" = "-w" ] && [ "$2" = "$FAKE_RO" ] && [ ! -e "$MOUNT_LOG.rw" ]; then return 1; fi; command test "$@"; }` + "\n"
		}
		input = append([]byte(prelude), input...)
	}
	return f.shellFixture.run(command, input, limit)
}

func useTestDrives(t *testing.T) (dir string, restore func()) {
	t.Helper()
	dir = t.TempDir()
	for _, sub := range []string{"mntdata", "mntshare", "tx"} {
		if err := os.MkdirAll(filepath.Join(dir, sub), 0755); err != nil {
			t.Fatal(err)
		}
	}
	saved := []string{gamesMount, gamesRoot, shareMount, gamesStaging}
	gamesMount = testShellPath(filepath.Join(dir, "mntdata"))
	gamesRoot = gamesMount + "/ID6Games"
	shareMount = testShellPath(filepath.Join(dir, "mntshare"))
	gamesStaging = shareMount + "/ID6Games-staging"
	return dir, func() { gamesMount, gamesRoot, shareMount, gamesStaging = saved[0], saved[1], saved[2], saved[3] }
}

func TestGameFilesGoToTheHardDriveAndAreNotUploadedTwice(t *testing.T) {
	dir, restore := useTestDrives(t)
	defer restore()
	f := gamesFixture{shellFixture: shellFixture{dir: dir}, mountLog: filepath.Join(dir, "mount.log")}
	tx := testShellPath(filepath.Join(dir, "tx"))
	gameNotes = nil
	if err := installGameData(f, tx, "Freedoom"); err != nil {
		t.Fatal(err)
	}
	for _, name := range gameFiles("Freedoom") {
		got, err := os.ReadFile(filepath.Join(dir, "mntdata", "ID6Games", "Freedoom", name))
		if err != nil || string(got) != "game data "+name {
			t.Fatalf("%s on the hard drive: %q, %v", name, got, err)
		}
		if _, err := os.Stat(filepath.Join(dir, "mntdata", "ID6Games", "Freedoom", name+".new")); !os.IsNotExist(err) {
			t.Fatal("a .new file was left behind")
		}
	}
	if entries, _ := os.ReadDir(filepath.Join(dir, "mntshare", "ID6Games-staging")); len(entries) != 0 {
		t.Fatalf("staging was not cleaned up: %v", entries)
	}
	if _, err := os.Stat(f.mountLog); !os.IsNotExist(err) {
		t.Fatal("a writable drive must not be remounted")
	}
	// second install: nothing uploaded, nothing staged
	gameNotes = nil
	if err := installGameData(f, tx, "Freedoom"); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(strings.Join(gameNotes, "\n"), "already on the hard drive, unchanged") || strings.Contains(strings.Join(gameNotes, "\n"), "copied") {
		t.Fatalf("unchanged files should be skipped: %v", gameNotes)
	}
	if _, err := os.Stat(filepath.Join(dir, "mntshare", "ID6Games-staging")); err == nil {
		if entries, _ := os.ReadDir(filepath.Join(dir, "mntshare", "ID6Games-staging")); len(entries) != 0 {
			t.Fatalf("nothing should be staged for unchanged files: %v", entries)
		}
	}
}

func TestReadOnlyHardDriveIsAlwaysSetBack(t *testing.T) {
	dir, restore := useTestDrives(t)
	defer restore()
	f := gamesFixture{shellFixture: shellFixture{dir: dir}, mountLog: filepath.Join(dir, "mount.log"), readOnly: true}
	staged := filepath.Join(dir, "mntshare", "game.wad")
	if err := os.WriteFile(staged, []byte("payload"), 0644); err != nil {
		t.Fatal(err)
	}
	ops := testShellPath(filepath.Join(dir, "tx")) + "/" + fileOpsName
	// success: writable for the copy, read-only again afterwards
	out, err := f.run("/bin/sh -s", gameFilesScript(ops, "Freedoom", []stagedFile{{testShellPath(staged), "game.wad", fnv64([]byte("payload")), 7}}), 1024)
	if err != nil || !strings.Contains(string(out), "mode=ro") || !strings.Contains(string(out), "done") {
		t.Fatalf("copy on a read-only drive: %q, %v", out, err)
	}
	log, _ := os.ReadFile(f.mountLog)
	if string(log) != "mount -u -o rw "+gamesMount+"\nmount -u -o ro "+gamesMount+"\n" {
		t.Fatalf("mount calls: %q", log)
	}
	// failure (wrong hash): the drive is still set back, and the bad copy is not left in place
	os.Remove(f.mountLog)
	if _, err := f.run("/bin/sh -s", gameFilesScript(ops, "Freedoom", []stagedFile{{testShellPath(staged), "bad.wad", "0000000000000000", 7}}), 1024); err == nil {
		t.Fatal("a hash mismatch must fail")
	}
	log, _ = os.ReadFile(f.mountLog)
	if !strings.HasSuffix(string(log), "mount -u -o ro "+gamesMount+"\n") {
		t.Fatalf("the drive was not set back after a failure: %q", log)
	}
	for _, left := range []string{"bad.wad", "bad.wad.new"} {
		if _, err := os.Stat(filepath.Join(dir, "mntdata", "ID6Games", "Freedoom", left)); !os.IsNotExist(err) {
			t.Fatalf("%s was left on the hard drive", left)
		}
	}
	// uninstall removes the game's folder the same way
	os.Remove(f.mountLog)
	gameNotes = nil
	if err := removeGameData(f, "Freedoom"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, "mntdata", "ID6Games", "Freedoom")); !os.IsNotExist(err) {
		t.Fatal("the game folder was not removed")
	}
	log, _ = os.ReadFile(f.mountLog)
	if !strings.HasSuffix(string(log), "mount -u -o ro "+gamesMount+"\n") {
		t.Fatalf("the drive was not set back after removing: %q", log)
	}
}

func TestFreedoomSavesComeBackFromAnOldUninstallBackup(t *testing.T) {
	dir := t.TempDir()
	older := filepath.Join(dir, "AppInstaller-Backup-0000000000000001")
	newer := filepath.Join(dir, "AppInstaller-Backup-0000000000000002")
	for _, d := range []string{older, newer} {
		if err := os.MkdirAll(filepath.Join(d, "settings-Freedoom", "Freedoom-data", ".savegame"), 0755); err != nil {
			t.Fatal(err)
		}
	}
	os.WriteFile(filepath.Join(older, "settings-Freedoom", "Freedoom-data", ".savegame", "doomsav0.dsg"), []byte("old"), 0644)
	os.WriteFile(filepath.Join(newer, "settings-Freedoom", "Freedoom-data", ".savegame", "doomsav0.dsg"), []byte("new"), 0644)
	past := mustParseTime(t, "2026-01-01T00:00:00Z")
	os.Chtimes(older, past, past)
	f := shellFixture{dir: dir}
	out, err := rescueFreedoomSaves(f, root+"/AppInstaller-Backup-ffffffffffffffff")
	if err != nil || !strings.HasPrefix(out, "restored") {
		t.Fatalf("rescue: %q, %v", out, err)
	}
	got, err := os.ReadFile(filepath.Join(dir, "Freedoom-data", ".savegame", "doomsav0.dsg"))
	if err != nil || string(got) != "new" {
		t.Fatalf("the newest saves should come back: %q, %v", got, err)
	}
	// saves already in place are never touched
	if out, err := rescueFreedoomSaves(f, root+"/AppInstaller-Backup-ffffffffffffffff"); err != nil || out != "present" {
		t.Fatalf("second rescue: %q, %v", out, err)
	}
}

func TestGamesInstallerOnlyKnowsGames(t *testing.T) {
	if len(apps) != 1 || apps[0].name != "GameRoom" {
		t.Fatalf("the games installer must only handle games: %v", apps)
	}
	if len(appExtraFiles) != 0 || strings.Join(legacyAppNames, " ") != "Freedoom Tyrian LibreQuake Retro" {
		t.Fatal("no other app's files or folders may be listed (only the separate game entries Game Room replaced)")
	}
	if strings.Join(appPayloadExtras("GameRoom"), " ") != "tyrian-qnx quake-qnx nes-qnx gb-qnx ps1-qnx psp-qnx ppsspp_libretro.so" || strings.Join(gameDataDirs("GameRoom"), " ") != "Freedoom Tyrian LibreQuake Retro PSP" {
		t.Fatal("Game Room ships the other two engines and keeps the three games' folders on the hard drive")
	}
	if len(appPayloadExtras("Retro")) != 0 {
		t.Fatal("Retro is no app of its own any more (its emulators are Game Room's)")
	}
	if len(appPayloadExtras("Freedoom")) != 0 || len(gameFiles("Freedoom")) != 2 || len(gameFiles("Tyrian")) == 0 {
		t.Fatal("Freedoom's data files belong on the hard drive, not in its plugin folder")
	}
}

func mustParseTime(t *testing.T, s string) time.Time {
	t.Helper()
	v, err := time.Parse(time.RFC3339, s)
	if err != nil {
		t.Fatal(err)
	}
	return v
}

// Game Room replaces the separate entries: their plugin folders go, their saves stay (game files on the hard
// drive are not touched by this step at all).
func TestOldSeparateGameEntriesMakeWayForGameRoom(t *testing.T) {
	dir := t.TempDir()
	for _, p := range oldGamePlugins {
		live := filepath.Join(dir, "Plugins", p.name)
		if err := os.MkdirAll(live, 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(live, p.name+".manifest"), []byte("<plugin><name>"+p.name+"</name></plugin>"), 0644); err != nil {
			t.Fatal(err)
		}
	}
	save := filepath.Join(dir, "Freedoom-data", ".savegame", "doomsav0.dsg")
	if err := os.MkdirAll(filepath.Dir(save), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(save, []byte("save"), 0644); err != nil {
		t.Fatal(err)
	}
	f := shellFixture{dir: dir}
	old, err := presentOldGamePlugins(f)
	if err != nil || len(old) != 4 {
		t.Fatalf("present: %v %v", old, err)
	}
	if _, removed, err := uninstall(f, old); err != nil || strings.Join(removed, " ") != "Freedoom Tyrian LibreQuake Retro" {
		t.Fatalf("replace: %v %v", removed, err)
	}
	for _, p := range oldGamePlugins {
		if _, err := os.Stat(filepath.Join(dir, "Plugins", p.name)); !os.IsNotExist(err) {
			t.Fatalf("%s is still installed", p.name)
		}
	}
	if got, err := os.ReadFile(save); err != nil || string(got) != "save" {
		t.Fatal("the Freedoom save was lost", err)
	}
	if old, _ := presentOldGamePlugins(f); len(old) != 0 {
		t.Fatal("still reported present")
	}
}

// After the car wiped every file under ID5 but kept the folders, an empty plugin folder must not block an install.
func TestEmptyPluginFolderIsCleared(t *testing.T) {
	dir := t.TempDir()
	empty := filepath.Join(dir, "Plugins", "GameRoom")
	full := filepath.Join(dir, "Plugins", "OtherApp")
	for _, d := range []string{empty, full} {
		if err := os.MkdirAll(d, 0755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(full, "OtherApp.manifest"), []byte("x"), 0644); err != nil {
		t.Fatal(err)
	}
	f := shellFixture{dir: dir}
	for _, name := range []string{"GameRoom", "OtherApp"} {
		if _, err := f.run(emptyDirRemoval(root+"/Plugins/"+name, hostFileOps), nil, 512); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := os.Stat(empty); !os.IsNotExist(err) {
		t.Fatal("the empty folder was kept")
	}
	if _, err := os.Stat(filepath.Join(full, "OtherApp.manifest")); err != nil {
		t.Fatal("a folder with files was touched")
	}
}
