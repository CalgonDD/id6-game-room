package main

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"testing/fstest"
	"time"
)

var hostFileOps string

func TestMain(m *testing.M) {
	os.Exit(func() int {
		dir, err := os.MkdirTemp("", "gameroom-fileops-test-")
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		defer os.RemoveAll(dir)
		hostFileOps = filepath.Join(dir, fileOpsName)
		compiler := os.Getenv("HOST_CC")
		if compiler == "" {
			compiler = "cc"
		}
		cmd := exec.Command(compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-o", filepath.ToSlash(hostFileOps), "../source/native/fileops.c")
		if out, err := cmd.CombinedOutput(); err != nil {
			fmt.Fprintf(os.Stderr, "build file operations test helper: %v\n%s", err, out)
			return 1
		}
		hostFileOps = testShellPath(hostFileOps)
		if err := loadTestPayload(); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		return m.Run()
	}())
}

// Windows tests use Cygwin's POSIX compiler and shell; the application and
// race detector still run as native Windows Go processes.
func testShellPath(path string) string {
	if runtime.GOOS != "windows" {
		return path
	}
	out, err := exec.Command("cygpath", "-u", path).Output()
	if err != nil {
		panic(err)
	}
	return strings.TrimSpace(string(out))
}

func executableOnHost(path string) bool {
	return exec.Command("sh", "-c", "test -x "+testShellPath(path)).Run() == nil
}

func loadTestPayload() error {
	payload := fstest.MapFS{
		"payload/tools/" + fileOpsName: {Data: []byte("host-test helper")},
	}
	for _, app := range apps {
		prefix := "payload/" + app.name + "/"
		for _, name := range []string{app.name + ".manifest", app.name + ".xml"} {
			data, err := os.ReadFile(filepath.Join("..", "source", "native", "plugin", name))
			if err != nil {
				return err
			}
			payload[prefix+name] = &fstest.MapFile{Data: data}
		}
		for _, name := range append([]string{app.name + "_ALL_Images.zip", app.name + "_ALL_Texts.zip", "lib" + app.name + ".so", app.worker}, appPayloadExtras(app.name)...) {
			payload[prefix+name] = &fstest.MapFile{Data: []byte(name)}
		}
	}
	for _, name := range []string{"freedoom1.wad", "freedoom2.wad"} {
		payload["payload/games/Freedoom/"+name] = &fstest.MapFile{Data: []byte("game data " + name)}
	}
	for _, name := range []string{"Thwaite.nes", "LICENSES.md", "uCity.gbc"} {
		payload["payload/games/Retro/"+name] = &fstest.MapFile{Data: []byte("game data " + name)}
	}
	for _, name := range []string{"pak0.pak", "pak1.pak"} {
		payload["payload/games/LibreQuake/"+name] = &fstest.MapFile{Data: []byte("game data " + name)}
	}
	for _, name := range []string{"tyrian1.lvl", "music.mus"} {
		payload["payload/games/Tyrian/"+name] = &fstest.MapFile{Data: []byte("game data " + name)}
	}
	bundled = payload
	return nil
}

type identifyOnly struct {
	version string
	t       *testing.T
}

func (r identifyOnly) run(cmd string, _ []byte, _ int) ([]byte, error) {
	r.t.Helper()
	if cmd != "cat /opt/sys/etc/nbt_version.txt" {
		r.t.Fatalf("unexpected command on blocked firmware: %s", cmd)
	}
	return []byte(r.version + "\r\n"), nil
}

func TestUnsupportedFirmwareStopsBeforePayloadCheck(t *testing.T) {
	for _, version := range []string{"NBTevo_H16205L", "NBTevo_M17345I", "NBTevo_N18032I", "NBTevo_Z24000I"} {
		got := inspect(identifyOnly{version, t}, vehicleInput{}, checkGateway)
		if got.Ready || got.Version != version || !strings.Contains(got.Result, "outside") {
			t.Errorf("%s: %+v", version, got)
		}
	}
	got := inspect(identifyOnly{"NBTevo_N18092I", t}, vehicleInput{Version: "NBTevo_Y24073J"}, checkGateway)
	if got.Ready || !strings.Contains(got.Result, "differs") {
		t.Fatal(got)
	}
}

type shellFixture struct {
	dir                 string
	failMapRename       bool
	corruptStagedRead   bool
	failUninstallRename bool
	failUnlock          bool
}

func (f shellFixture) run(command string, input []byte, _ int) ([]byte, error) {
	// Simulate the head unit with real local shell/filesystem semantics.
	const missingUtilities = "printf() { return 126; }\nls() { return 126; }\nrmdir() { echo 'rmdir: cannot execute - No such file or directory' >&2; return 126; }\nmv() { return 126; }\n"
	shellDir := testShellPath(f.dir)
	cmd := strings.ReplaceAll(command, root, shellDir)
	if strings.HasSuffix(command, "/"+fileOpsName+" check") {
		start := strings.LastIndex(cmd, " && ")
		cmd = cmd[:start] + " && " + hostFileOps + " check"
	}
	if f.corruptStagedRead && strings.HasPrefix(command, "cat ") && strings.HasSuffix(command, "/new-GameRoom/GameRoom.xml") {
		return []byte("truncated upload"), nil
	}
	if strings.HasPrefix(command, "find ") {
		cmd += " | sed 's|" + shellDir + "|" + root + "|'"
	}
	if command == "/bin/sh -s" {
		input = bytes.ReplaceAll(input, []byte(root), []byte(shellDir))
		input = bytes.ReplaceAll(input, []byte(`"$ops"`), []byte("test_fileops"))
		guard := ""
		if f.failMapRename {
			guard = `case "$1:$2:$3" in move:*/new-GameRoom:*/Plugins/GameRoom) return 1;; esac; `
		}
		if f.failUninstallRename {
			guard = `case "$1:$2:$3" in move:*/Plugins/GameRoom:*/removed-GameRoom) return 1;; esac; `
		}
		if f.failUnlock {
			guard += `if test "$1" = unlock; then return 1; fi; `
		}
		input = append([]byte(missingUtilities+"test_fileops() { "+guard+hostFileOps+" \"$@\"; }\n"), input...)
	}
	process := exec.Command("sh", "-c", missingUtilities+cmd)
	process.Stdin = bytes.NewReader(input)
	out, err := process.Output()
	if failure, ok := err.(*exec.ExitError); ok {
		return out, fmt.Errorf("%w: %s", err, bytes.TrimSpace(failure.Stderr))
	}
	return out, err
}

func TestStagedReadbackMismatchLeavesLivePluginsAlone(t *testing.T) {
	dir := t.TempDir()
	live := filepath.Join(dir, "Plugins", "GameRoom")
	if err := os.MkdirAll(live, 0755); err != nil {
		t.Fatal(err)
	}
	manifest := []byte("<plugin><name>GameRoom</name></plugin>")
	path := filepath.Join(live, "GameRoom.manifest")
	if err := os.WriteFile(path, manifest, 0644); err != nil {
		t.Fatal(err)
	}
	_, err := install(shellFixture{dir: dir, corruptStagedRead: true}, "169.254.21.3", panel{1280, 480}, false, apps)
	if err == nil || !strings.Contains(err.Error(), "upload verification failed for GameRoom.xml") {
		t.Fatalf("expected upload verification failure, got %v", err)
	}
	actual, err := os.ReadFile(path)
	if err != nil || !bytes.Equal(actual, manifest) {
		t.Fatal("existing plugin was changed", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "ID6-install-lock")); !os.IsNotExist(err) {
		t.Fatal("publication started after a failed staged readback", err)
	}
}

func TestFreedoomSavesSurviveReinstall(t *testing.T) {
	dir := t.TempDir()
	tx := filepath.Join(dir, "AppInstaller-Backup-0123456789abcdef")
	live := filepath.Join(dir, "Plugins", "Freedoom")
	stage := filepath.Join(tx, "new-Freedoom")
	write := func(path, text string) {
		t.Helper()
		if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(text), 0755); err != nil {
			t.Fatal(err)
		}
	}
	for _, folder := range []string{live, stage} {
		write(filepath.Join(folder, "Freedoom.manifest"), "<plugin><name>Freedoom</name></plugin>")
		write(filepath.Join(folder, "freedoom-qnx"), "#!/bin/sh\n")
	}
	write(filepath.Join(live, ".savegame", "doomsav1.dsg"), "old save")
	write(filepath.Join(live, "default.cfg"), "old settings")
	write(filepath.Join(dir, "Freedoom-data", "doomgenericdoom.cfg"), "kept")
	write(filepath.Join(live, "doomgenericdoom.cfg"), "must not overwrite")
	fixture := shellFixture{dir: dir}
	freedoom := []plugin{oldGamePlugins[0]} // the separate Freedoom entry's publication (still the code path)
	if _, err := fixture.run("/bin/sh -s", publicationScript(root+"/AppInstaller-Backup-0123456789abcdef", freedoom), 1024); err != nil {
		t.Fatal(err)
	}
	for path, want := range map[string]string{
		filepath.Join(dir, "Freedoom-data", ".savegame", "doomsav1.dsg"): "old save",
		filepath.Join(dir, "Freedoom-data", "default.cfg"):               "old settings",
		filepath.Join(dir, "Freedoom-data", "doomgenericdoom.cfg"):       "kept",
	} {
		got, err := os.ReadFile(path)
		if err != nil || string(got) != want {
			t.Fatalf("%s: %q, %v", path, got, err)
		}
	}
	if _, err := os.Stat(filepath.Join(live, ".savegame")); !os.IsNotExist(err) {
		t.Fatal("the new plugin folder should not hold the saves", err)
	}
}

func TestFreedoomKeepsNoBackups(t *testing.T) {
	dir := t.TempDir()
	older := filepath.Join(dir, "AppInstaller-Backup-0123456789abcdef")
	unfinished := filepath.Join(dir, "AppInstaller-Backup-1111111111111111")
	onlyFreedoom := filepath.Join(dir, "AppInstaller-Backup-2222222222222222")
	tx := filepath.Join(dir, "AppInstaller-Backup-fedcba9876543210")
	for _, path := range []string{
		filepath.Join(older, "old-Freedoom"), filepath.Join(older, "old-OtherApp"),
		filepath.Join(unfinished, "old-Freedoom"),
		filepath.Join(onlyFreedoom, "removed-Freedoom"),
		filepath.Join(tx, "old-Freedoom"), filepath.Join(tx, "old-SecondApp"),
	} {
		if err := os.MkdirAll(path, 0755); err != nil {
			t.Fatal(err)
		}
	}
	for _, done := range []string{older, onlyFreedoom} {
		if err := os.WriteFile(filepath.Join(done, "complete"), nil, 0644); err != nil {
			t.Fatal(err)
		}
	}
	fixture := shellFixture{dir: dir}
	// before staging: finished transactions lose their Freedoom backups; one left empty goes entirely
	if _, err := fixture.run("/bin/sh -s", []byte(noBackupPurge(root+"/AppInstaller-Backup-fedcba9876543210")), 2048); err != nil {
		t.Fatal(err)
	}
	gone := func(path string) bool { _, err := os.Stat(path); return os.IsNotExist(err) }
	if !gone(filepath.Join(older, "old-Freedoom")) || gone(filepath.Join(older, "old-OtherApp")) {
		t.Fatal("the purge must remove only Freedoom's backup from a finished transaction")
	}
	if !gone(onlyFreedoom) {
		t.Fatal("a transaction holding only a Freedoom backup should be removed")
	}
	if gone(filepath.Join(unfinished, "old-Freedoom")) || gone(filepath.Join(tx, "old-Freedoom")) {
		t.Fatal("unfinished transactions and the current one must not be touched before publication")
	}
	// after the install: this transaction's own Freedoom copy goes, other apps' backups stay
	freedoom := []plugin{oldGamePlugins[0]} // the separate Freedoom entry's publication (still the code path)
	if _, err := fixture.run("/bin/sh -s", backupRetentionScript(root+"/AppInstaller-Backup-fedcba9876543210", freedoom), 2048); err != nil {
		t.Fatal(err)
	}
	if !gone(filepath.Join(tx, "old-Freedoom")) || gone(filepath.Join(tx, "old-SecondApp")) || gone(filepath.Join(older, "old-OtherApp")) {
		t.Fatal("after the install only Freedoom's copies may be removed")
	}
	if gone(filepath.Join(unfinished, "old-Freedoom")) {
		t.Fatal("an unfinished transaction must never be touched")
	}
}

func TestPublicationLeavesAnExistingLockAlone(t *testing.T) {
	dir := t.TempDir()
	lock := filepath.Join(dir, "ID6-install-lock")
	if err := os.Mkdir(lock, 0755); err != nil {
		t.Fatal(err)
	}
	fixture := shellFixture{dir: dir}
	if _, err := fixture.run("/bin/sh -s", publicationScript(root+"/AppInstaller-Backup-test", apps), 1024); err == nil || !strings.Contains(err.Error(), "lock already exists") {
		t.Fatalf("expected a lock error, got %v", err)
	}
	if _, err := os.Stat(lock); err != nil {
		t.Fatal(err)
	}
}

func TestBlockedInstallReportsPreviousTransactionWithoutChanges(t *testing.T) {
	dir := t.TempDir()
	lock := filepath.Join(dir, "ID6-install-lock")
	tx := filepath.Join(dir, "AppInstaller-Backup-ae68da9e4a9d7ec9")
	for _, path := range []string{lock, filepath.Join(tx, "old-GameRoom"), filepath.Join(tx, "new-GameRoom"), filepath.Join(dir, "Plugins", "GameRoom")} {
		if err := os.MkdirAll(path, 0755); err != nil {
			t.Fatal(err)
		}
	}
	marker := filepath.Join(tx, "fresh-GameRoom")
	if err := os.WriteFile(marker, nil, 0644); err != nil {
		t.Fatal(err)
	}
	newTx, err := install(shellFixture{dir: dir}, "169.254.21.3", panel{1280, 480}, true, apps)
	if newTx != "" || err == nil {
		t.Fatalf("expected a read-only lock report, got %q, %v", newTx, err)
	}
	for _, line := range []string{"Lock type: directory", "Live app paths:", "GameRoom: directory", "AppInstaller-Backup-ae68da9e4a9d7ec9", "old-GameRoom: directory", "new-GameRoom: directory", "fresh-GameRoom: file"} {
		if !strings.Contains(err.Error(), line) {
			t.Fatalf("report missing %q: %v", line, err)
		}
	}
	for _, path := range []string{lock, filepath.Join(tx, "old-GameRoom"), marker} {
		if _, err := os.Stat(path); err != nil {
			t.Fatal(err)
		}
	}
	transactions, err := filepath.Glob(filepath.Join(dir, "AppInstaller-Backup-????????????????"))
	if err != nil || len(transactions) != 1 || transactions[0] != tx {
		t.Fatalf("blocked attempt created another transaction: %v, %v", transactions, err)
	}
}

func (r compatibilityFixture) run(cmd string, _ []byte, _ int) ([]byte, error) {
	switch cmd {
	case "cat /opt/sys/etc/nbt_version.txt":
		return []byte(r.version), nil
	case "uname -a":
		return []byte("QNX hu-omap 6.5.0 2015/12/01 TI-OMAP5432-uEVM armle\n"), nil
	case "cat /opt/hmi/ID5/lib/" + appFramework:
		return r.app, nil
	case "cat /opt/hmi/ID5/lib/" + coreFramework:
		return r.core, nil
	case "/opt/sys/bin/LayerManagerControl get screen 0":
		return []byte(r.display), nil
	}
	if strings.HasPrefix(cmd, "for f in ") {
		return []byte("#Coded is: def,1180_480,touch_screen\n"), nil
	}
	if cmd == writableCheck {
		return nil, nil
	}
	r.t.Fatalf("unexpected check: %s", cmd)
	return nil, nil
}

type compatibilityFixture struct {
	identifyOnly
	app, core []byte
	display   string
}

func TestInspectionRequiresMatchingFrameworksAndActivePanel(t *testing.T) {
	const version = "NBTevo_V20153I"
	fixture := compatibilityFixture{
		identifyOnly: identifyOnly{version, t},
		app:          []byte("app framework"), core: []byte("core framework"),
		display: "- resolution: x=1440, y=540\n",
	}
	saved := profiles[version]
	profiles[version] = frameworkPair{digest(fixture.app), digest(fixture.core)}
	defer func() { profiles[version] = saved }()
	input := vehicleInput{Head: "169.254.21.10", Gateway: "169.254.21.3"}
	reportsHead := func(_, _ string) (string, error) { return input.Head, nil }
	got := inspect(fixture, input, reportsHead)
	if !got.Ready || got.screen != (panel{1440, 540}) || !strings.Contains(got.Touch, "Touch coded") {
		t.Fatal(got)
	}
	mismatch := fixture
	mismatch.core = []byte("other core")
	if got := inspect(mismatch, input, reportsHead); got.Ready || !strings.Contains(got.Result, coreFramework) {
		t.Fatal(got)
	}
	fixture.display = ""
	if got := inspect(fixture, input, reportsHead); got.Ready || got.Display != "Unknown" {
		t.Fatal(got)
	}
	input.DisplayResolution = "1280x480"
	if got := inspect(fixture, input, reportsHead); !got.Ready || got.screen != (panel{1280, 480}) || !strings.Contains(got.Display, "manual") {
		t.Fatal(got)
	}
	if got := inspect(mismatch, input, reportsHead); got.Ready {
		t.Fatal(got)
	}
	fixture.display = "- resolution: x=1920, y=720\n"
	if got := inspect(fixture, input, reportsHead); got.Ready || !strings.Contains(got.Result, "differs") {
		t.Fatal(got)
	}
	input.DisplayResolution = "1920x720"
	if got := inspect(fixture, input, reportsHead); !got.Ready || got.screen != (panel{1920, 720}) || !strings.Contains(got.Display, "Experimental") {
		t.Fatal(got)
	}
	input.DisplayResolution = ""
	if got := inspect(fixture, input, reportsHead); got.Ready || got.Display != "1920 × 720" {
		t.Fatal(got)
	}
	fixture.display = ""
	input.DisplayResolution = "1600x600"
	if got := inspect(fixture, input, reportsHead); !got.Ready || got.screen != (panel{1600, 600}) {
		t.Fatal(got)
	}
	input.DisplayResolution = "800x720"
	if got := inspect(fixture, input, reportsHead); got.Ready || !strings.Contains(got.Result, "aspect ratio") {
		t.Fatal(got)
	}
	input.DisplayResolution = ""
	fixture.display = "- resolution: x=800, y=480\n"
	if got := inspect(fixture, input, reportsHead); !got.Ready || got.screen != (panel{800, 480}) {
		t.Fatal(got)
	}
	if got := inspect(fixture, input, func(_, _ string) (string, error) { return "", fmt.Errorf("connection refused") }); got.Ready || !strings.Contains(got.Result, "port 6801") {
		t.Fatal(got)
	}
	if got := inspect(fixture, input, func(_, _ string) (string, error) { return "169.254.21.100", nil }); got.Ready || !strings.Contains(got.Result, "different head unit") {
		t.Fatal(got)
	}
}

// Large files (freedoom1.wad) go up in verified pieces, and the final size is checked with the staged helper.
func TestChunkedUploadVerifiesEachPiece(t *testing.T) {
	saved := uploadChunk
	uploadChunk = 64
	defer func() { uploadChunk = saved }()
	dir := t.TempDir()
	if err := os.Mkdir(filepath.Join(dir, "tx"), 0755); err != nil {
		t.Fatal(err)
	}
	data := make([]byte, 1000)
	for i := range data {
		data[i] = byte(i * 7)
	}
	fixture := shellFixture{dir: dir}
	ops, path := root+"/tx/"+fileOpsName, root+"/tx/freedoom1.wad"
	if err := uploadFile(fixture, ops, path, data); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(filepath.Join(dir, "tx", "freedoom1.wad"))
	if err != nil || !bytes.Equal(got, data) {
		t.Fatalf("uploaded file differs (%v, %d bytes)", err, len(got))
	}
	if _, err := os.Stat(filepath.Join(dir, "tx", "freedoom1.wad.part")); !os.IsNotExist(err) {
		t.Fatal("piece file left behind", err)
	}
	if err := checkRemoteSize(fixture, ops, path, len(data)); err != nil {
		t.Fatal(err)
	}
	if err := checkRemoteSize(fixture, ops, path, len(data)+1); err == nil {
		t.Fatal("a wrong size was accepted")
	}
	// The same file again must not overwrite silently (set -C), as for small files.
	if err := uploadFile(fixture, ops, path, data); err == nil {
		t.Fatal("an existing staged file was overwritten")
	}
}

// A lock left by an install whose staging failed (car, 2026-11-19) is recoverable even with apps installed,
// but only while the newest transaction holds nothing but new-* staging folders.
func TestFailedStagingLockIsRecoverable(t *testing.T) {
	dir := t.TempDir()
	lock := filepath.Join(dir, "ID6-install-lock")
	older := filepath.Join(dir, "AppInstaller-Backup-0123456789abcdef")
	newest := filepath.Join(dir, "AppInstaller-Backup-fedcba9876543210")
	for _, path := range []string{lock, filepath.Join(dir, "Plugins", "Freedoom"), filepath.Join(older, "old-Freedoom"),
		filepath.Join(newest, "new-Freedoom"), filepath.Join(newest, "new-Freedoom")} {
		if err := os.MkdirAll(path, 0755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(dir, "Plugins", "Freedoom", "Freedoom.manifest"), []byte("x"), 0644); err != nil {
		t.Fatal(err)
	}
	past := time.Now().Add(-time.Hour)
	if err := os.Chtimes(older, past, past); err != nil {
		t.Fatal(err)
	}
	fixture := shellFixture{dir: dir}
	_, err := install(fixture, "169.254.21.3", panel{1280, 480}, false, apps)
	if blocked, ok := err.(*blockedInstall); !ok || !blocked.canRecover {
		t.Fatalf("expected a recoverable failed staging: %v", err)
	}
	// A publication marker in the newest transaction means something live may have moved: no recovery.
	marker := filepath.Join(newest, "old-Freedoom")
	if err := os.Mkdir(marker, 0755); err != nil {
		t.Fatal(err)
	}
	_, err = install(fixture, "169.254.21.3", panel{1280, 480}, false, apps)
	if blocked, ok := err.(*blockedInstall); !ok || blocked.canRecover {
		t.Fatalf("expected a publication marker to block recovery: %v", err)
	}
	if err := os.Remove(marker); err != nil {
		t.Fatal(err)
	}
	// Confirmed recovery gets past the lock (the rest of the install is covered by the other tests).
	_, err = install(fixture, "169.254.21.3", panel{1280, 480}, true, apps)
	if _, ok := err.(*blockedInstall); ok {
		t.Fatalf("recovery was still blocked: %v", err)
	}
}
