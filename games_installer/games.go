package main

// games.go: where Game Room's files go. The small HMI plugin goes on the flash like any app
// (Plugins/GameRoom, loaded by the iDrive as usual), but its big data files go on the hard drive, in
// /mnt/data/ID6Games/<Game>. The iDrive never opens that folder; only the game does, when it is started.
//
// /mnt/data is BMW's main data partition. BMW's create_hdd.sh defines it read-only and only switches it to
// writable for a moment to create its own folders, then back; we do the same, file by file: each file is
// uploaded and checked on /mnt/share (always writable) first, then one short shell, with an exit trap that
// always puts the drive back the way it was found, copies it in, checks it and renames it into place.
// CSI_NBT's recovery manager rebuilds a partition whose file system is found corrupt and leaves
// PARTITION_REFORMATTED.txt; we report that marker, since a rebuild would also remove the games.

import (
	"time"

	"fmt"
	"hash/fnv"
	"io/fs"
	"strings"
)

var (
	gamesMount   = "/mnt/data"
	gamesRoot    = gamesMount + "/ID6Games"
	shareMount   = "/mnt/share"
	gamesStaging = shareMount + "/ID6Games-staging"
	// rebuild markers CSI_NBT's recovery manager leaves after re-creating a partition
	rebuildMarkers = []string{"/mnt/data/PARTITION_REFORMATTED.txt", "/mnt/quota/sys/PARTITION_REFORMATTED.txt"}
)

// gameDataDirs are the folders under ID6Games an app's game files go in: Game Room's three engines keep their
// own (the same folders the separate apps used, so nothing moves on the hard drive).
func gameDataDirs(app string) []string {
	if app == "GameRoom" {
		return []string{"Freedoom", "Tyrian", "LibreQuake", "Retro", "PSP"}
	}
	return []string{app}
}

// gameFiles lists a game's data files: everything embedded under payload/games/<Game>/ (staged by
// tools/stage_payload.py). A game with none keeps everything in its plugin.
func gameFiles(game string) []string {
	var names []string
	base := "payload/games/" + game + "/"
	_ = fs.WalkDir(bundled, strings.TrimSuffix(base, "/"), func(path string, e fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !e.IsDir() {
			names = append(names, strings.TrimPrefix(path, base))
		}
		return nil
	})
	return names
}

// fnv64 matches `gameroom-fileops hash`.
func fnv64(data []byte) string {
	h := fnv.New64a()
	h.Write(data)
	return fmt.Sprintf("%016x", h.Sum64())
}

func shellQuote(s string) string { return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'" }

// remoteHash asks the staged helper for a file's hash on the car.
func remoteHash(r remoteRunner, ops, path string) (string, error) {
	return remoteHashSized(r, ops, path, 0)
}

// remoteHashSized gives a big file's hash the time reading it takes on the car (size in bytes; 0 = small).
func remoteHashSized(r remoteRunner, ops, path string, size int) (string, error) {
	script := "ops=" + shellQuote(ops) + "\n\"$ops\" hash " + shellQuote(path) + " 2>/dev/null\n"
	out, err := runTimed(r, "/bin/sh -s", []byte(script), 64, carTime(size))
	return strings.TrimSpace(string(out)), err
}

// carTime is a generous time budget for the car to read or copy size bytes on its hard drive (about 5 MB/s).
func carTime(size int) time.Duration {
	return 8*time.Second + time.Duration(size/(5<<20))*time.Second
}

// runTimed runs a command with its own time budget where the connection allows one (the SSH link does).
func runTimed(r remoteRunner, command string, input []byte, limit int, budget time.Duration) ([]byte, error) {
	if timed, ok := r.(interface {
		runDeadline(string, []byte, int, time.Duration) ([]byte, error)
	}); ok {
		return timed.runDeadline(command, input, limit, budget)
	}
	return r.run(command, input, limit)
}

// streamUpload sends a big file in one go (a PS1 disc is 300-700 MB: piece by piece that was thousands of SSH
// round trips). Only on the real SSH link; the caller checks the size and hash afterwards.
func streamUpload(r remoteRunner, path string, data []byte) (bool, error) {
	timed, ok := r.(interface {
		runDeadline(string, []byte, int, time.Duration) ([]byte, error)
	})
	if !ok {
		return false, nil
	}
	budget := 30*time.Second + time.Duration(len(data)/(1<<20))*time.Second // at least 1 MB/s
	_, err := timed.runDeadline("umask 022; set -C; cat > "+shellQuote(path), data, 512, budget)
	return true, err
}

// gameNotes collects what the last install did on the hard drive, for the result shown to the user.
var gameNotes []string

func gameNote(format string, args ...any) {
	gameNotes = append(gameNotes, fmt.Sprintf(format, args...))
}

// rebuildMarkerReport lists any partition-rebuild markers on the car (read only).
func rebuildMarkerReport(r remoteRunner) string {
	var script strings.Builder
	for _, m := range rebuildMarkers {
		fmt.Fprintf(&script, "if test -e %[1]s; then echo %[1]s; fi\n", m)
	}
	out, err := r.run("/bin/sh -s", []byte(script.String()), 1024)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(out))
}

// rescueFreedoomSaves puts Freedoom's saves back if an earlier uninstall moved them into a backup
// (settings-Freedoom/Freedoom-data in an AppInstaller-Backup-* folder). Runs before the old
// Freedoom backups are deleted. The newest copy wins; saves already in place are never touched.
func rescueFreedoomSaves(r remoteRunner, tx string) (string, error) {
	script := "root=" + root + "\nops=" + shellQuote(tx+"/"+fileOpsName) + "\n" + `data="$root/Freedoom-data"
if test -e "$data" || test -L "$data"; then echo present; exit 0; fi
newest=
for d in "$root"/AppInstaller-Backup-????????????????; do
    test -d "$d/settings-Freedoom/Freedoom-data" && test ! -L "$d/settings-Freedoom/Freedoom-data" || continue
    if test -z "$newest" || test "$d" -nt "$newest"; then newest=$d; fi
done
if test -z "$newest"; then echo none; exit 0; fi
"$ops" move "$newest/settings-Freedoom/Freedoom-data" "$data" || exit 1
echo "restored $newest"
`
	out, err := runMutation(r, "/bin/sh -s", []byte(script), 1024)
	return strings.TrimSpace(string(out)), err
}

type stagedFile struct {
	staged, name, want string
	size               int
}

// gameFilesScript publishes a game's checked, staged files into its folder on the hard drive, all in one
// short writable window. It is the only thing that ever writes to /mnt/data, and it always leaves the drive
// as it found it.
func gameFilesScript(ops, game string, files []stagedFile) []byte {
	// game is the folder under ID6Games: "Freedoom", or an own-games folder like "Retro/yours".
	var body strings.Builder
	fmt.Fprintf(&body, `ops=%s
mnt=%s
top=%s
dir=%s
made_rw=0
fail() { echo "$1" >&2; exit 1; }
restore() {
    if test "$made_rw" = 1; then
        mount -u -o ro "$mnt" || echo "WARNING: $mnt could not be set back to read-only; restarting iDrive does that." >&2
    fi
}
trap restore 0
trap 'exit 1' HUP INT TERM
test -d "$mnt" || fail "The hard drive ($mnt) is not mounted."
if test -w "$mnt"; then
    echo mode=rw
else
    mount -u -o rw "$mnt" || fail "Could not make the hard drive writable."
    made_rw=1
    echo mode=ro
fi
test -d "$top" || mkdir "$top" || fail "Could not create $top."
put() {
    rm -f "$dir/$2.new"
    "$ops" copy "$1" "$dir/$2.new" || fail "Could not copy $2 to the hard drive."
    got=$("$ops" hash "$dir/$2.new") || fail "Could not read back $2 on the hard drive."
    if test "$got" != "$3"; then rm -f "$dir/$2.new"; fail "$2 differs after copying to the hard drive."; fi
    rm -f "$dir/$2"
    "$ops" move "$dir/$2.new" "$dir/$2" || fail "Could not put $2 in place."
}
`, shellQuote(ops), shellQuote(gamesMount), shellQuote(gamesRoot), shellQuote(gamesRoot+"/"+game))
	// the game's folder, one level at a time (the car has no mkdir -p)
	sub := gamesRoot
	for _, part := range strings.Split(game, "/") {
		sub += "/" + part
		fmt.Fprintf(&body, "test -d %[1]s || mkdir %[1]s || fail %[2]s\n", shellQuote(sub), shellQuote("Could not create "+sub+"."))
	}
	for _, f := range files {
		// Pinned PPSSPP assets have nested paths. Only create descendants of
		// this game's folder; never interpret a payload name as a shell path.
		parts := strings.Split(f.name, "/")
		parent := gamesRoot + "/" + game
		for _, part := range parts[:len(parts)-1] {
			if part == "" || part == "." || part == ".." {
				return []byte("echo 'Invalid game asset path' >&2; exit 1\n")
			}
			parent += "/" + part
			fmt.Fprintf(&body, "test -d %[1]s || mkdir %[1]s || fail %[2]s\n", shellQuote(parent), shellQuote("Could not create game asset folder."))
		}
		if last := parts[len(parts)-1]; last == "" || last == "." || last == ".." {
			return []byte("echo 'Invalid game asset name' >&2; exit 1\n")
		}
		fmt.Fprintf(&body, "put %s %s %s\n", shellQuote(f.staged), shellQuote(f.name), shellQuote(f.want))
	}
	body.WriteString("echo done\n")
	return []byte(body.String())
}

// putItem is one file for putGameFiles: its name in the folder and how to read it.
type putItem struct {
	name string
	load func() ([]byte, error)
}

// installGameData puts the game's data files on the hard drive, skipping any already there unchanged.
func installGameData(r remoteRunner, tx, game string) error {
	var items []putItem
	for _, name := range gameFiles(game) {
		name := name
		items = append(items, putItem{name: name, load: func() ([]byte, error) {
			data, err := fs.ReadFile(bundled, "payload/games/"+game+"/"+name)
			if err != nil {
				return nil, fmt.Errorf("embedded game file %s is missing: %w", name, err)
			}
			return data, nil
		}})
	}
	return putGameFiles(r, tx, game, items)
}

// putGameFiles puts files into a folder under the games folder on the hard drive (dir: "Retro", or
// "Retro/yours" for the owner's own games), skipping any already there unchanged. Each file is uploaded to
// /mnt/share under a plain numbered name (game names have spaces, which the upload commands can't take),
// checked, then all are published in one writable window under their real names.
func putGameFiles(r remoteRunner, tx, dir string, items []putItem) error {
	if len(items) == 0 {
		return nil
	}
	label := strings.ReplaceAll(dir, "/", " ")
	ops := tx + "/" + fileOpsName
	target := gamesRoot + "/" + dir
	if _, err := r.run("test -d "+shellQuote(gamesMount), nil, 128); err != nil {
		return fmt.Errorf("the hard drive (%s) is not mounted, so %s's game files cannot be installed", gamesMount, label)
	}
	if out, err := r.run("if test -w "+shellQuote(gamesMount)+"; then echo rw; else echo ro; fi", nil, 128); err == nil {
		gameNote("Hard drive %s was %s before installing %s.", gamesMount, map[string]string{"rw": "writable", "ro": "read-only"}[strings.TrimSpace(string(out))], label)
	}
	stageDir := gamesStaging + "/" + strings.TrimPrefix(tx[strings.LastIndex(tx, "/")+1:], backupPrefix) + "-" + strings.ReplaceAll(dir, "/", "-")
	staged := false
	defer func() {
		if staged {
			_, _ = r.run("rm -rf "+shellQuote(stageDir), nil, 512)
		}
	}()
	var todo []stagedFile
	unchanged := 0
	for i, item := range items {
		data, err := item.load()
		if err != nil {
			return fmt.Errorf("could not read %s: %w", item.name, err)
		}
		want := fnv64(data)
		if got, err := remoteHash(r, ops, target+"/"+item.name); err == nil && got == want {
			unchanged++
			continue
		}
		if !staged {
			script := fmt.Sprintf("test -d %[1]s && test -w %[1]s || exit 3\ntest -d %[2]s || mkdir %[2]s || exit 1\nmkdir %[3]s || exit 1\n",
				shellQuote(shareMount), shellQuote(gamesStaging), shellQuote(stageDir))
			if _, err := r.run("/bin/sh -s", []byte(script), 256); err != nil {
				return fmt.Errorf("could not prepare the staging folder on %s: %w", shareMount, err)
			}
			staged = true
		}
		path := fmt.Sprintf("%s/f%04d", stageDir, i)
		streamed := false
		if len(data) > 8<<20 {
			var err error
			if streamed, err = streamUpload(r, path, data); err != nil {
				return fmt.Errorf("could not upload %s: %w", item.name, err)
			}
		}
		if !streamed {
			if err := uploadFile(r, ops, path, data); err != nil {
				return fmt.Errorf("could not upload %s: %w", item.name, err)
			}
		}
		if got, err := remoteHashSized(r, ops, path, len(data)); err != nil || got != want {
			return fmt.Errorf("upload verification failed for %s", item.name)
		}
		todo = append(todo, stagedFile{path, item.name, want, len(data)})
	}
	if unchanged > 0 {
		gameNote("%s: %d file(s) already on the hard drive, unchanged (not uploaded again).", label, unchanged)
	}
	if len(todo) == 0 {
		return nil
	}
	total := 0
	for _, item := range todo {
		total += item.size
	}
	var out []byte
	var err error
	if total > 32<<20 { // big files: copying and checking them takes longer than the usual budget
		out, err = runTimed(r, "/bin/sh -s", gameFilesScript(ops, dir, todo), 1024, 45*time.Second+3*carTime(total))
	} else {
		out, err = runMutation(r, "/bin/sh -s", gameFilesScript(ops, dir, todo), 1024)
	}
	if err != nil {
		return fmt.Errorf("could not put %s's files on the hard drive: %w", label, err)
	}
	if strings.Contains(string(out), "mode=ro") {
		check, err := r.run("if test -w "+shellQuote(gamesMount)+"; then echo rw; else echo ro; fi", nil, 128)
		if err != nil || strings.TrimSpace(string(check)) != "ro" {
			gameNote("WARNING: %s is still writable after copying %s's files; restarting iDrive sets it back to read-only.", gamesMount, label)
		} else {
			gameNote("%s: %d file(s) copied to the hard drive (made writable for the copy, back to read-only).", label, len(todo))
		}
	} else {
		gameNote("%s: %d file(s) copied to the hard drive.", label, len(todo))
	}
	return nil
}

// removeGameData deletes a game's files from the hard drive (uninstall). Saves are on the flash and stay, and
// so do the owner's own games (yours/), which only "Remove ticked games" deletes.
func removeGameData(r remoteRunner, game string) error {
	if len(gameFiles(game)) == 0 {
		return nil
	}
	dir := gamesRoot + "/" + game
	script := fmt.Sprintf(`mnt=%s
dir=%s
test -e "$dir" || { echo absent; exit 0; }
kept=
test -d "$dir/yours" && kept=1
made_rw=0
restore() { if test "$made_rw" = 1; then mount -u -o ro "$mnt" || echo "WARNING: $mnt could not be set back to read-only; restarting iDrive does that." >&2; fi; }
trap restore 0
trap 'exit 1' HUP INT TERM
if ! test -w "$mnt"; then mount -u -o rw "$mnt" || { echo "Could not make the hard drive writable." >&2; exit 1; }; made_rw=1; fi
for f in "$dir"/* "$dir"/.[!.]*; do
    test -e "$f" || test -L "$f" || continue
    test "$f" = "$dir/yours" && continue
    rm -rf "$f" || exit 1
done
test -n "$kept" || rm -rf "$dir" || exit 1
echo "removed${kept:+ kept}"
`, shellQuote(gamesMount), shellQuote(dir))
	out, err := runMutation(r, "/bin/sh -s", []byte(script), 1024)
	if err != nil {
		return err
	}
	switch strings.TrimSpace(string(out)) {
	case "removed":
		gameNote("%s's game files were removed from the hard drive. Saves were kept.", game)
	case "removed kept":
		gameNote("%s's game files were removed from the hard drive. Saves and your own added games were kept.", game)
	}
	return nil
}

// gamesDiagnostics is the diagnostics section for games: how the hard drive is mounted, what is installed
// there, rebuild markers, saves, and staging left behind.
const gamesDiagnostics = `echo '=== games: hard drive ==='
if test -d /mnt/data; then
    if test -w /mnt/data; then echo '/mnt/data: writable'; else echo '/mnt/data: read-only'; fi
else
    echo '/mnt/data: not mounted'
fi
echo
echo '=== games: files on the hard drive ==='
find /mnt/data/ID6Games 2>&1
echo
echo '=== games: partition rebuild markers ==='
for m in /mnt/data/PARTITION_REFORMATTED.txt /mnt/quota/sys/PARTITION_REFORMATTED.txt; do
    if test -e "$m"; then echo "FOUND $m:"; cat "$m" 2>&1; else echo "none at $m"; fi
done
echo
echo '=== games: saves and settings (flash) ==='
find /var/opt/hmi/data/ID5/Freedoom-data /var/opt/hmi/data/ID5/Tyrian-data /var/opt/hmi/data/ID5/LibreQuake-data /var/opt/hmi/data/ID5/Retro-data /var/opt/hmi/data/ID5/PSP-data 2>&1
echo
echo '=== games: flash (/var: plugins and saves; df in 512-byte blocks) ==='
df /var/opt/hmi/data/ID5 2>&1
for d in Freedoom-data Tyrian-data LibreQuake-data Retro-data PSP-data; do du -s /var/opt/hmi/data/ID5/$d 2>&1; done
ls -l /var/opt/hmi/data/ID5/Retro-data /var/opt/hmi/data/ID5/PSP-data 2>&1
echo
echo '=== games: staging left on /mnt/share ==='
find /mnt/share/ID6Games-staging 2>&1
echo
echo '=== games: plugin file inventory (read-only) ==='
find /var/opt/hmi/data/ID5/Plugins -type f 2>&1
inventory_status=$?
echo "plugin_inventory_status=$inventory_status"
echo
echo '=== games: recovery evidence fileArchive.log (last 400 lines) ==='
if test -f /var/dump/fileArchive.log; then
    tail -400 /var/dump/fileArchive.log 2>&1
    archive_status=$?
    echo "fileArchive_read_status=$archive_status"
else
    echo 'fileArchive.log: not present'
fi
echo '=== games: recovery evidence end ==='
`
