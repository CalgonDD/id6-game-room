package main

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"embed"
	"encoding/hex"
	"encoding/xml"
	"errors"
	"fmt"
	"io/fs"
	"net"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"golang.org/x/crypto/ssh"
)

//go:embed payload
var embeddedPayload embed.FS

var bundled fs.FS = embeddedPayload

const (
	root        = "/var/opt/hmi/data/ID5"
	installLock = root + "/ID6-install-lock"
	// backupPrefix names each run's backup/staging folder (plus 16 hex digits).
	backupPrefix = "AppInstaller-Backup-"
	fileOpsName  = "gameroom-fileops"
)

// writableCheck mirrors the check inspect() uses to decide whether the live
// Plugins folder can actually be published to. It does NOT require Plugins
// to already exist: on a car that has never had any HMI plugin installed,
// it simply isn't there yet, and install() creates it on demand - only
// root (ID5) itself needs to be writable so that creation can happen.
const writableCheck = "test -w " + root + " && { test ! -e " + root + "/Plugins || { test -d " + root + "/Plugins && test -w " + root + "/Plugins; }; }"

// remountCandidates are tried from most to least specific: NBT Evo units
// commonly mount /var (or a sub-path of it) read-only by default, and the
// exact mount point varies by firmware/variant, so this just tries each
// plausible one rather than requiring the car's own `mount` output format
// to be parsed.
var remountCandidates = []string{root, "/var/opt/hmi/data", "/var/opt/hmi", "/var/opt", "/var"}

// remountWritable tries to remount whichever filesystem backs the Plugins
// folder as read-write. It is only ever run when the user explicitly asks
// for it (a tick box), never automatically during inspect/install, since
// flipping a read-only safety mount on a real vehicle is a deliberate
// choice, not something to do silently.
//
// If every candidate fails, the returned error carries real diagnostics
// pulled from the car (the mount table and each directory's permissions)
// instead of just "it didn't work" - the exact mount point and remount
// syntax vary enough across NBT Evo firmware/variants that guessing twice
// isn't worth another round trip.
func remountWritable(r remoteRunner) (string, error) {
	isWritable := func() bool {
		_, err := r.run(writableCheck, nil, 128)
		return err == nil
	}
	if isWritable() {
		return "", nil
	}
	var attempts []string
	for _, mountpoint := range remountCandidates {
		if _, err := r.run("mount -uw "+mountpoint, nil, 2048); err != nil {
			attempts = append(attempts, mountpoint+": "+err.Error())
			continue
		}
		if isWritable() {
			return mountpoint, nil
		}
		attempts = append(attempts, mountpoint+": remounted but still not writable")
	}
	diagnostics, diagErr := r.run(remountDiagnosticsScript, nil, 16384)
	if diagErr != nil {
		return "", fmt.Errorf("none of the remounts worked (%s); could not collect diagnostics: %w", strings.Join(attempts, "; "), diagErr)
	}
	return "", fmt.Errorf("none of the remounts worked:\n%s\n\n%s", strings.Join(attempts, "\n"), strings.TrimSpace(string(diagnostics)))
}

const remountDiagnosticsScript = `echo '--- mount ---'
mount
echo '--- permissions ---'
for p in /var /var/opt /var/opt/hmi /var/opt/hmi/data /var/opt/hmi/data/ID5 /var/opt/hmi/data/ID5/Plugins; do
    ls -ld "$p" 2>&1
done
`

// diagSection is one tick-box in the Diagnostics panel: a read-only shell
// snippet whose output becomes part of the report.
type diagSection struct {
	key, label, script string
}

// diagSections are the selectable parts of "Pull diagnostics". Every snippet
// only reads; none changes anything on the car.
var diagSections = []diagSection{
	{"games", "Games: hard drive, game files, rebuild markers, saves", gamesDiagnostics},
	{"system", "System, display and install state", `echo '=== uname ==='
uname -a
echo
echo '=== firmware version file ==='
cat /opt/sys/etc/nbt_version.txt 2>&1
echo
echo '=== mount ==='
mount
echo
echo '=== display ==='
/opt/sys/bin/LayerManagerControl get screen 0 2>&1
echo
echo '=== directory permissions ==='
for p in /var /var/opt /var/opt/hmi /var/opt/hmi/data /var/opt/hmi/data/ID5 /var/opt/hmi/data/ID5/Plugins /opt/hmi/ID5/lib; do
    ls -ld "$p" 2>&1
done
echo
echo '=== Plugins contents ==='
ls -la /var/opt/hmi/data/ID5/Plugins 2>&1
echo
echo '=== install lock ==='
if test -e ` + installLock + ` || test -L ` + installLock + `; then echo present; ls -la ` + installLock + ` 2>&1; else echo absent; fi
echo
echo '=== staged transactions ==='
ls -ld ` + root + `/AppInstaller-Backup-???????????????? 2>&1
echo
echo '=== disk space ==='
df 2>&1
echo
`},
	{"processes", "HMI threads and the game's processes", `echo '=== HmiMain3D threads ==='
pidin -p HmiMain3D -F "%a %b %N %J %B" 2>&1
echo
echo '=== HmiMain3D memory map (library load addresses) ==='
pidin -p HmiMain3D mem 2>&1
echo
echo '=== game processes (freedoom-qnx, tyrian-qnx, quake-qnx, nes-qnx, gb-qnx, and audioconn while the sound source is being set up) ==='
pidin 2>&1 | grep -iE 'freedoom|tyrian|quake|nes-qnx|gb-qnx|audioconn'
echo
`},
	{"syslog", "System log (last 200 lines)", `echo '=== recent system log (last 200 lines) ==='
sloginfo 2>&1 | tail -200
echo
`},
	// Each big wd_err/wd_warn log is a dump of every thread of every process at the moment the
	// watchdog fired (process, thread, priority, state, what it is blocked on). A plain tail used
	// to start partway through another process, so HmiMain3D's own rows - what each HMI thread was
	// doing when it was killed - never reached a report. Now: the column header line and every
	// HmiMain3D row. The whole files are saved next to report.txt (fetchWatchdogLogs, which runs
	// first), so the report no longer repeats a 60-line start and a 400-line tail of each one: with
	// a dozen logs on the unit that pushed this section past its 45 s budget on 2026-11-18 and
	// took the SSH connection - and every step after it - down. `head` is not on this unit's
	// non-interactive PATH, so sed takes the header.
	{"watchdog", "Crash dump list and watchdog freeze reports", `echo '=== crash dumps ==='
ls -la /var/dump 2>&1
echo
echo '=== watchdog freeze reports ==='
for f in /var/dump/wd_err_*.log /var/dump/wd_warn_*.log; do
    test -f "$f" || continue
    echo "-- $f --"
    case "$f" in
    *_Systemload.log|*_mem.log)
        cat "$f" 2>&1
        echo
        continue;;
    esac
    echo '(first line of the file: the column header)'
    sed -n '1p' "$f" 2>&1
    echo '(every HmiMain3D line in this file: what each HMI thread was doing when the watchdog fired)'
    grep 'HmiMain3D' "$f" 2>&1
    echo "(grep status $?: 1 means no HmiMain3D line in this file)"
    echo
done
echo '(each log is also saved whole under watchdog\ in this pull folder)'
echo
`},
	{"deps", "Game library dependencies", `echo '=== game plugin and program dependencies ==='
for bin in ` + root + `/Plugins/GameRoom/libGameRoom.so ` + root + `/Plugins/GameRoom/freedoom-qnx ` + root + `/Plugins/GameRoom/tyrian-qnx ` + root + `/Plugins/GameRoom/quake-qnx ` + root + `/Plugins/GameRoom/nes-qnx ` + root + `/Plugins/GameRoom/gb-qnx ` + root + `/Plugins/GameRoom/ps1-qnx ` + root + `/Plugins/GameRoom/psp-qnx ` + root + `/Plugins/GameRoom/ppsspp_libretro.so; do
    echo "-- $bin --"
    ldd "$bin" 2>&1
done
echo
`},
	{"trace", "Game events and launches (shared trace)", `echo '=== HMI trace: epoch wall time, monotonic time, PID/TID, sequence/loss and call entry/return ==='
for name in HMI-apps-trace.previous.txt HMI-apps-trace.txt; do
    f="` + root + `/"$name
    echo "=== $name ==="
    if test -f "$f"; then
        ls -la "$f" 2>&1
        if ! dd if="$f" bs=1048576 count=1 2>/dev/null; then
            echo "(dd could not read $f)"
        fi
    else
        echo "(not present - trace sink not started, no records, or no rotation yet)"
    fi
    echo
done
`},
}

// diagnosticsScript wraps one section's snippet as its own read-only script.
// The report is best-effort: a missing app or file must not make the whole
// section fail (the shell would otherwise exit with the last command's status).
func diagnosticsScript(section diagSection) string {
	return section.script + "exit 0\n"
}

// pullDiagnostics runs every selected section as its own bounded SSH call
// (instead of one concatenated script under one shared deadline) and, if
// chosen, appends the framework libraries' hashes. It never changes anything
// on the car.
//
// Each section gets runLong's full 45s budget rather than a share of one
// fixed deadline for the whole pull: a slow section (sloginfo, a core-dump
// ldd pass) used to eat into every other section's time, and picking more
// sections made the same shared deadline more likely to be hit by whichever
// section ran last. Running sections independently also means one section
// timing out no longer destroys the sections that already came back -
// pullDiagnostics returns everything collected so far, plus a non-nil error
// naming which section(s) did not finish, instead of discarding the whole
// report. Callers decide whether a partial report is still worth keeping.
func pullDiagnostics(r longRunner, selected map[string]bool) (string, error) {
	var report strings.Builder
	var incomplete []string
	ran := false
	for _, section := range diagSections {
		if section.key == "hashes" || !selected[section.key] {
			continue
		}
		ran = true
		fmt.Fprintf(&report, "=== %s ===\n", section.label)
		out, err := r.runLong(diagnosticsScript(section), nil, 10<<20)
		report.Write(out)
		if err != nil {
			incomplete = append(incomplete, section.label)
			fmt.Fprintf(&report, "\n(%s did not finish: %v)\n", section.label, err)
		}
		report.WriteString("\n")
	}
	if selected["hashes"] {
		ran = true
		report.WriteString("=== framework hashes ===\n")
		for _, name := range []string{appFramework, coreFramework} {
			raw, err := r.run("cat /opt/hmi/ID5/lib/"+name, nil, 4<<20)
			if err != nil {
				fmt.Fprintf(&report, "%s: could not read (%v)\n", name, err)
				incomplete = append(incomplete, "Framework hashes")
				continue
			}
			fmt.Fprintf(&report, "%s: %s\n", name, digest(raw))
		}
		report.WriteString("\n=== known-good hashes (for comparison) ===\n")
		for version, pair := range profiles {
			fmt.Fprintf(&report, "%s: app=%s core=%s\n", version, pair.app, pair.core)
		}
	}
	if !ran {
		return "", errors.New("no diagnostics sections selected")
	}
	if len(incomplete) > 0 {
		return report.String(), fmt.Errorf("some sections did not finish: %s", strings.Join(incomplete, ", "))
	}
	return report.String(), nil
}

// crashDumpCandidates are the core dumps a pull downloads, every one present: the games' own programs first (a
// game crashing is the usual case; the PS1 one's core was left out of the 2026-11-20 17:25 pull because only the
// HMI's was ever fetched), then HmiMain3D (a fault in the HMI itself points at a loaded plugin).
var crashDumpCandidates = []string{
	"/var/dump/ps1-qnx.core",
	"/var/dump/psp-qnx.core",
	"/var/dump/gb-qnx.core",
	"/var/dump/nes-qnx.core",
	"/var/dump/freedoom-qnx.core",
	"/var/dump/tyrian-qnx.core",
	"/var/dump/quake-qnx.core",
	"/var/dump/HmiMain3D.core",
	"/var/dump/TunerApp.core",
}

type crashDump struct {
	name string
	data []byte
}

// fetchCrashDumps downloads every core dump of crashDumpCandidates that is on the car. A missing file is not
// an error - most diagnostics pulls won't have one.
func fetchCrashDumps(r longRunner) ([]crashDump, error) {
	var dumps []crashDump
	for _, path := range crashDumpCandidates {
		present, checkErr := r.run("if test -e "+path+"; then echo present; fi", nil, 32)
		if checkErr != nil {
			return dumps, fmt.Errorf("could not check crash dump: %w", checkErr)
		}
		if strings.TrimSpace(string(present)) != "present" {
			continue
		}
		out, runErr := r.runLong("cat "+path, nil, 32<<20)
		if runErr != nil {
			return dumps, fmt.Errorf("could not download %s: %w", path, runErr)
		}
		if len(out) == 0 {
			continue
		}
		dumps = append(dumps, crashDump{filepath.Base(path), out})
	}
	return dumps, nil
}

// watchdogLogPath is what fetchWatchdogLogs will download: only the watchdog's own logs in
// /var/dump, so a listing can never steer a `cat` anywhere else.
var watchdogLogPath = regexp.MustCompile(`^/var/dump/wd_(err|warn)_[A-Za-z0-9_.-]+\.log$`)

// fetchWatchdogLogs downloads every /var/dump/wd_err_*.log and wd_warn_*.log whole. The report
// keeps only parts of them (see the "watchdog" section); the full dump of every thread of every
// process at the moment the watchdog fired is the one record of what HmiMain3D was blocked on.
// `cat` over SSH is byte-exact - it is how crash dumps already come back - so nothing here relies
// on bsdtar/gzip being on the unit's non-interactive PATH. Read-only on the car.
func fetchWatchdogLogs(r remoteRunner) (logs map[string][]byte, failed []string, err error) {
	list, err := r.run(`for f in /var/dump/wd_err_*.log /var/dump/wd_warn_*.log; do test -f "$f" && echo "$f"; done; exit 0`, nil, 64<<10)
	if err != nil {
		return nil, nil, fmt.Errorf("could not list watchdog logs: %w", err)
	}
	logs = map[string][]byte{}
	for _, p := range strings.Fields(string(list)) {
		name := strings.TrimPrefix(p, "/var/dump/")
		if !watchdogLogPath.MatchString(p) {
			failed = append(failed, name+" (unexpected name, skipped)")
			continue
		}
		out, runErr := runMutation(r, "cat "+p, nil, 16<<20)
		if runErr != nil {
			failed = append(failed, name)
			continue
		}
		logs[name] = out
	}
	return logs, failed, nil
}

// carLibraries are the libraries HmiMain3D loads on NBTevo_V20153I, at the
// exact paths found on the car (Car Lib Grabber, 2026-11-11). They let a
// crash dump be read with the car's own code instead of the QNX SDK's.
// libc lives at /usr/lib/ldqnx.so.2 on this unit, so it is also saved as
// libc.so.3, the name the dump refers to it by.
var carLibraries = []struct{ name, path string }{
	{"ldqnx.so.2", "/usr/lib/ldqnx.so.2"},
	{"libecpp-ne.so.4", "/fs/sda0/lib/libecpp-ne.so.4"},
	{"libcpp-ne.so.4", "/fs/sda0/lib/libcpp-ne.so.4"},
	{"libstdc++.so.6", "/fs/sda0/lib/libstdc++.so.6"},
	{"libm.so.2", "/fs/sda0/lib/libm.so.2"},
	{"libsocket.so.3", "/fs/sda0/lib/libsocket.so.3"},
	{"libAppFramework.A.so", "/fs/sda0/opt/hmi/ID5/lib/libAppFramework.A.so"},
	{"libCoreFramework.A.so", "/fs/sda0/opt/hmi/ID5/lib/libCoreFramework.A.so"},
	{"libBMWOTFapi.so.3", "/fs/sda0/opt/hmi/otf/libBMWOTFapi.so.3"},
	{"libDevIL.so", "/fs/sda0/opt/hmi/ID5/lib/libDevIL.so"},
	{"libEGL.so.1", "/fs/sda0/lib/libEGL.so.1"},
	{"libEGL_X.gpp.so", "/fs/sda0/opt/sys/lib/libEGL_X.gpp.so"},
	{"libGLESv2.so.1", "/fs/sda0/lib/libGLESv2.so.1"},
	{"libGLESv2_X_FORWARD.gpp.so", "/fs/sda0/opt/sys/lib/libGLESv2_X_FORWARD.gpp.so"},
	{"libILU.so", "/fs/sda0/opt/hmi/ID5/lib/libILU.so"},
	{"libIMGegl.so", "/fs/sda0/lib/graphics/omap5430_ES2_0-debug/libIMGegl.so"},
	{"libImgGLESv2.so", "/fs/sda0/lib/graphics/omap5430_ES2_0-debug/libImgGLESv2.so"},
	{"libKD.so.1", "/fs/sda0/lib/libKD.so.1"},
	{"libKD_X.gpp.so", "/fs/sda0/opt/sys/lib/libKD_X.gpp.so"},
	{"libOpenCTM.so", "/fs/sda0/opt/hmi/ID5/lib/libOpenCTM.so"},
	{"libbacktrace.so.1", "/fs/sda0/lib/libbacktrace.so.1"},
	{"libboost_filesystem.so", "/fs/sda0/opt/hmi/ID5/lib/libboost_filesystem.so"},
	{"libboost_regex.so", "/fs/sda0/opt/hmi/ID5/lib/libboost_regex.so"},
	{"libboost_system.so", "/fs/sda0/opt/hmi/ID5/lib/libboost_system.so"},
	{"libcommonapi21_someip.gpp.so.7", "/fs/sda0/opt/sys/lib/libcommonapi21_someip.gpp.so.7"},
	{"libcommunication.gpp.so", "/fs/sda0/opt/sys/lib/libcommunication.gpp.so"},
	{"libcrypto.so.2", "/fs/sda0/lib/libcrypto.so.2"},
	{"libdce.so.1", "/fs/sda0/lib/libdce.so.1"},
	{"libfreetype2.so", "/fs/sda0/opt/hmi/ID5/lib/libfreetype2.so"},
	{"libfribidi.so", "/fs/sda0/opt/hmi/ID5/lib/libfribidi.so"},
	{"libglslcompiler.so", "/fs/sda0/lib/graphics/omap5430_ES2_0-debug/libglslcompiler.so"},
	{"libharfbuzz.so", "/fs/sda0/opt/hmi/ID5/lib/libharfbuzz.so"},
	{"libheartbeat.so.3", "/fs/sda0/lib/libheartbeat.so.3"},
	{"libhmia4a.so", "/fs/sda0/opt/hmi/ID5/data/ro/hmiplugins/libhmia4a.so"},
	{"libhmibase.so", "/fs/sda0/opt/hmi/ID5/lib/libhmibase.so"},
	{"libhmicoding.so", "/fs/sda0/opt/hmi/ID5/lib/libhmicoding.so"},
	{"libhmicom.so", "/fs/sda0/opt/hmi/ID5/lib/libhmicom.so"},
	{"libhmidb.so", "/fs/sda0/opt/hmi/ID5/lib/libhmidb.so"},
	{"libhmifwsupport.so", "/fs/sda0/opt/hmi/ID5/lib/libhmifwsupport.so"},
	{"libhmigraphicbase.so", "/fs/sda0/opt/hmi/ID5/lib/libhmigraphicbase.so"},
	{"libhmilog.so", "/fs/sda0/opt/hmi/ID5/lib/libhmilog.so"},
	{"libhmiperformancemonitor.so", "/fs/sda0/opt/hmi/ID5/lib/libhmiperformancemonitor.so"},
	{"libhmiplugin.so", "/fs/sda0/opt/hmi/ID5/lib/libhmiplugin.so"},
	{"libhmiregistry.so", "/fs/sda0/opt/hmi/ID5/lib/libhmiregistry.so"},
	{"libhmiremoting.so", "/fs/sda0/opt/hmi/ID5/data/ro/hmiplugins/libhmiremoting.so"},
	{"libhmispeechdialog.so", "/fs/sda0/opt/hmi/ID5/lib/libhmispeechdialog.so"},
	{"libhmispeechtypes.so", "/fs/sda0/opt/hmi/ID5/lib/libhmispeechtypes.so"},
	{"libhmiwebserverservice.so", "/fs/sda0/opt/hmi/ID5/lib/libhmiwebserverservice.so"},
	{"libhmizip.so", "/fs/sda0/opt/hmi/ID5/lib/libhmizip.so"},
	{"libicui18n.so", "/fs/sda0/opt/hmi/ID5/lib/libicui18n.so"},
	{"libicuuc.so", "/fs/sda0/opt/hmi/ID5/lib/libicuuc.so"},
	{"libilmClient.gpp.so.1", "/fs/sda0/opt/sys/lib/libilmClient.gpp.so.1"},
	{"libiwnn.1.6.6-r1.so", "/fs/sda0/opt/hmi/ID5/lib/libiwnn.1.6.6-r1.so"},
	{"libmemmgr.so.1", "/fs/sda0/lib/libmemmgr.so.1"},
	{"libpvr2d.so", "/fs/sda0/lib/graphics/omap5430_ES2_0-debug/libpvr2d.so"},
	{"libscreen.so.1", "/fs/sda0/lib/libscreen.so.1"},
	{"libsrv_um.so", "/fs/sda0/lib/graphics/omap5430_ES2_0-debug/libsrv_um.so"},
	{"libssl.so.2", "/fs/sda0/lib/libssl.so.2"},
	{"libsys_colibry.gpp.so.7", "/fs/sda0/opt/sys/lib/libsys_colibry.gpp.so.7"},
	{"libsys_colibry_net.gpp.so.7", "/fs/sda0/opt/sys/lib/libsys_colibry_net.gpp.so.7"},
	{"libsys_dsi_base.gpp.so.4", "/fs/sda0/opt/sys/lib/libsys_dsi_base.gpp.so.4"},
	{"libsys_dsi_colibry.gpp.so.8", "/fs/sda0/opt/sys/lib/libsys_dsi_colibry.gpp.so.8"},
	{"libsys_dsi_commonapi21.gpp.so.2", "/fs/sda0/opt/sys/lib/libsys_dsi_commonapi21.gpp.so.2"},
	{"libsys_dsi_servicebroker.gpp.so.4", "/fs/sda0/opt/sys/lib/libsys_dsi_servicebroker.gpp.so.4"},
	{"libsys_dsi_trace.gpp.so.6", "/fs/sda0/opt/sys/lib/libsys_dsi_trace.gpp.so.6"},
	{"libsys_fwthirdparty_commonapi21.so", "/fs/sda0/opt/sys/lib/libsys_fwthirdparty_commonapi21.so"},
	{"libsys_someip.gpp.so.8", "/fs/sda0/opt/sys/lib/libsys_someip.gpp.so.8"},
	{"libsys_trace_clienttrace.gpp.so.7", "/fs/sda0/opt/sys/lib/libsys_trace_clienttrace.gpp.so.7"},
	{"libtilerusr.so.1", "/fs/sda0/lib/libtilerusr.so.1"},
	{"libusc.so", "/fs/sda0/lib/graphics/omap5430_ES2_0-debug/libusc.so"},
	{"libz.so.2", "/fs/sda0/lib/libz.so.2"},
	{"wsegl-screen.so", "/fs/sda0/lib/graphics/omap5430_ES2_0-debug/wsegl-screen.so"},
}

// fetchCarLibraries downloads carLibraries one by one, read-only, calling
// progress(done, total, name) before each file. Missing files are skipped and
// returned in missing; the result maps file name to contents.
func fetchCarLibraries(r longRunner, progress func(done, total int, name string)) (libs map[string][]byte, missing []string) {
	libs = map[string][]byte{}
	for i, lib := range carLibraries {
		if progress != nil {
			progress(i, len(carLibraries), lib.name)
		}
		out, err := r.runLong("test -r "+lib.path+" && cat "+lib.path, nil, 24<<20)
		if err != nil || len(out) == 0 {
			missing = append(missing, lib.name)
			continue
		}
		libs[lib.name] = out
		if lib.name == "ldqnx.so.2" {
			libs["libc.so.3"] = out
		}
	}
	return libs, missing
}

// deleteBackups (games installer) removes only the games' backups (old-/removed-/settings-<Game> in finished
// installer folders), never another plugin's. Games keep none anyway; this
// clears any left by older builds. It refuses while an install lock exists.
func deleteBackups(r remote) ([]string, error) {
	script := "lock=" + installLock + "\n" + `if test -e "$lock" || test -L "$lock"; then echo locked; exit 0; fi
for d in ` + root + `/AppInstaller-Backup-????????????????; do
    test -d "$d" && test -f "$d/complete" || continue
    for a in ` + strings.Join(noBackupApps, " ") + `; do
        for p in "$d/old-$a" "$d/removed-$a" "$d/settings-$a"; do
            if test -e "$p"; then echo "$p"; fi
        done
    done
done
` + noBackupPurge("")
	out, err := r.runLong("/bin/sh -s", []byte(script), 64<<10)
	if err != nil {
		return nil, err
	}
	text := strings.TrimSpace(string(out))
	if text == "locked" {
		return nil, errors.New("an install lock is present, so the backups may still be needed. Recover or finish the install first")
	}
	return strings.Fields(text), nil
}

type vehicleInput struct {
	Head, Gateway, Local, Version, Fingerprint, DisplayResolution, Password string
	Apps, UninstallApps                                                     []string
	OwnGames, RemoveGames                                                   []string // Your games: keys to add, car paths to remove
}

type inspection struct {
	Version, Result, Fingerprint, Display, Touch string
	Ready, CanRecover, CanUnlockEmpty            bool
	CarGames                                     []string // Your games on the car (Retro/yours/x.gbc ...)
	CarGamesListed                               bool
	screen                                       panel
}

var firmwarePattern = regexp.MustCompile(`(?i)\bNBTevo_([a-z][0-9]{5}[a-z])\b`)

func firmwareName(text string) string {
	m := firmwarePattern.FindStringSubmatch(text)
	if len(m) == 2 {
		return "NBTevo_" + strings.ToUpper(m[1])
	}
	return ""
}

// sshConnect retries the dial+handshake once more on a transient network
// failure - the reported real-world symptom ("ssh: handshake failed: ...
// i/o timeout") on this car's USB/ENET link is often a one-off hiccup, not a
// dead link, and every install/uninstall/diagnostics action starts here.
// It never retries an authentication or host-key rejection: those are not
// transient, and silently retrying past a changed host key or a wrong
// password would hide a real security decision rather than a flaky cable.
//
// 2026-11-18: install kept failing with "handshake failed ... i/o timeout"
// while Connect, Check compatibility and Pull diagnostics worked - the same
// single connection, so the head unit was simply slow to finish the
// handshake at those moments (the SSH server runs below the HMI's busy
// threads). A handshake changes nothing on the car, so it is always safe to
// wait longer and try again: sshHandshakeLimit per try, sshAttempts tries.
const (
	sshAttempts       = 4
	sshHandshakeLimit = 20 * time.Second
)

func sshConnect(in vehicleInput) (*ssh.Client, net.Conn, string, error) {
	var client *ssh.Client
	var socket net.Conn
	var fingerprint string
	var err error
	started := time.Now()
	for attempt := 1; attempt <= sshAttempts; attempt++ {
		tried := time.Now()
		client, socket, fingerprint, err = sshConnectOnce(in)
		if err != nil {
			logAction("SSH connect %s attempt %d/%d failed after %d ms: %v", in.Head, attempt, sshAttempts, time.Since(tried).Milliseconds(), err)
		} else {
			logAction("SSH connect %s attempt %d/%d ok in %d ms", in.Head, attempt, sshAttempts, time.Since(tried).Milliseconds())
		}
		if err == nil || !transientDialError(err) {
			return client, socket, fingerprint, err
		}
		if attempt == sshAttempts {
			return client, socket, fingerprint, fmt.Errorf("%w (%d tries over %d s: the head unit accepted the connection but answered too slowly; wait until iDrive has settled, with no app open, and try again)",
				err, sshAttempts, int(time.Since(started).Seconds()))
		}
		time.Sleep(time.Duration(attempt) * time.Second)
	}
	return client, socket, fingerprint, err
}

// transientDialError reports whether err looks like a network hiccup worth
// retrying (a timeout, reset or refused connection) rather than an auth or
// host-key rejection, which must always surface immediately.
func transientDialError(err error) bool {
	if err == nil {
		return false
	}
	msg := err.Error()
	if strings.Contains(msg, "not confirmed") || strings.Contains(msg, "SSH key changed") ||
		strings.Contains(msg, "unable to authenticate") || strings.Contains(msg, "password") {
		return false
	}
	var netErr net.Error
	if errors.As(err, &netErr) && netErr.Timeout() {
		return true
	}
	return strings.Contains(msg, "i/o timeout") || strings.Contains(msg, "connection reset") ||
		strings.Contains(msg, "connection refused") || strings.Contains(msg, "EOF") ||
		strings.Contains(msg, "could not reach head unit")
}

func sshConnectOnce(in vehicleInput) (*ssh.Client, net.Conn, string, error) {
	config := &ssh.ClientConfig{
		User: "root",
		Config: ssh.Config{
			KeyExchanges: append(append([]string{}, ssh.SupportedAlgorithms().KeyExchanges...),
				"diffie-hellman-group14-sha1", "diffie-hellman-group1-sha1"),
			Ciphers: append(append([]string{}, ssh.SupportedAlgorithms().Ciphers...), "aes128-cbc", "aes256-cbc", "3des-cbc"),
			MACs:    append(append([]string{}, ssh.SupportedAlgorithms().MACs...), "hmac-sha1", "hmac-sha1-96"),
		},
		HostKeyAlgorithms: append(append([]string{}, ssh.SupportedAlgorithms().HostKeys...), "ssh-rsa", "ssh-dss"),
	}
	if in.Fingerprint != "" {
		if in.Password == "" {
			return nil, nil, "", errors.New("Enter the root SSH password for the connected head unit.")
		}
		config.Auth = []ssh.AuthMethod{ssh.Password(in.Password)}
	}
	var observed string
	config.HostKeyCallback = func(_ string, _ net.Addr, key ssh.PublicKey) error {
		observed = ssh.FingerprintSHA256(key)
		if observed != in.Fingerprint {
			return errors.New("SSH fingerprint has not been confirmed")
		}
		return nil
	}
	c, err := enetDial(in.Local, in.Head, "22", 4*time.Second)
	if err != nil {
		return nil, nil, "", fmt.Errorf("could not reach head unit SSH: %w", err)
	}
	_ = c.SetDeadline(time.Now().Add(sshHandshakeLimit))
	conn, channels, requests, err := ssh.NewClientConn(c, net.JoinHostPort(in.Head, "22"), config)
	if err != nil {
		c.Close()
		return nil, nil, observed, err
	}
	return ssh.NewClient(conn, channels, requests), c, observed, nil
}

type remote struct {
	client *ssh.Client
	socket net.Conn
}

type bounded struct {
	data  bytes.Buffer
	limit int
}

func (b *bounded) Write(p []byte) (int, error) {
	if b.data.Len()+len(p) > b.limit {
		return 0, errors.New("SSH response exceeded expected size")
	}
	return b.data.Write(p)
}

func (r remote) run(cmd string, input []byte, limit int) ([]byte, error) {
	return r.runDeadline(cmd, input, limit, 8*time.Second)
}

// runLong is for commands that legitimately take longer than the normal
// 8-second budget, such as the multi-part diagnostics sweep (sloginfo,
// several ldd calls, pidin, mount, df all in one session) - especially on
// a car that's already under stress from whatever is being diagnosed.
// Without this, the SSH transport's own deadline fires mid-command and
// surfaces as a generic "remote command exited without exit status or
// exit signal" error that has nothing to do with the actual problem.
func (r remote) runLong(cmd string, input []byte, limit int) ([]byte, error) {
	return r.runDeadline(cmd, input, limit, 45*time.Second)
}

func (r remote) runDeadline(cmd string, input []byte, limit int, timeout time.Duration) ([]byte, error) {
	_ = r.socket.SetDeadline(time.Now().Add(timeout))
	s, err := r.client.NewSession()
	if err != nil {
		return nil, err
	}
	defer s.Close()
	out, stderr := bounded{limit: limit}, bounded{limit: 2048}
	s.Stdin = bytes.NewReader(input)
	s.Stdout, s.Stderr = &out, &stderr
	if err = s.Run(cmd); err != nil {
		if detail := strings.TrimSpace(stderr.data.String()); detail != "" {
			return out.data.Bytes(), fmt.Errorf("%w: %s", err, detail)
		}
		return out.data.Bytes(), err
	}
	return out.data.Bytes(), nil
}

const appFramework = "libAppFramework.A.so"
const coreFramework = "libCoreFramework.A.so"

type frameworkPair struct{ app, core string }

var profiles = map[string]frameworkPair{
	"NBTevo_N18092I": {
		"93f4c7ab7e59c0adf7923b9b491de06165ba9d97bbe7412292e7ddc418ca2123",
		"64ba154c68c772f9ca779fc18507f5b34715be58ded4a198b8a62b90d16c3ee0",
	},
	"NBTevo_V20153I": {
		"085b7f2a9f96734be51621a70a04c24d01525eb5e9852f45c845a7d2f4ae4c67",
		"56705e56c349d9ae57a7782bd39b795cfc01391ba8ff155046f32fbe8ffad2cb",
	},
	"NBTevo_Y24073J": {
		"ef2488b007d592301d8f21d9aa60771ad552d06b5e97c29e7813e3946644d820",
		"cb0e488d1ebe2e30de87bb56f0e93d6a2d76b44309b0b47fef5b0b7cec048973",
	},
}

type remoteRunner interface {
	run(cmd string, input []byte, limit int) ([]byte, error)
}

func inspect(r remoteRunner, in vehicleInput, gatewayCheck func(string, string) (string, error)) inspection {
	result := inspection{Fingerprint: in.Fingerprint}
	versionFile, _ := r.run("cat /opt/sys/etc/nbt_version.txt", nil, 1024)
	detected := firmwareName(string(versionFile))
	entered := firmwareName(in.Version)
	if in.Version != "" && !strings.EqualFold(in.Version, entered) {
		result.Result = "Enter the full NBTevo code shown on iDrive."
		return result
	}
	if detected != "" && entered != "" && entered != detected {
		result.Version = detected
		result.Result = "The entered version differs from the version reported over SSH."
		return result
	}
	result.Version = detected
	if result.Version == "" {
		result.Version = entered
	}
	if result.Version == "" {
		result.Result = "The head unit did not expose the screen version over SSH. Enter the full code shown in iDrive and check again."
		return result
	}
	profile, ok := profiles[result.Version]
	if !ok {
		result.Result = "This firmware is outside this three-car preview. Installation is disabled."
		return result
	}
	info, err := r.run("uname -a", nil, 1024)
	platform := strings.Fields(string(info))
	if err != nil || len(platform) < 4 || platform[0] != "QNX" || platform[2] != "6.5.0" || platform[len(platform)-1] != "armle" {
		result.Result = "The head unit must report QNX 6.5.0 ARM."
		return result
	}
	for _, framework := range []struct{ name, hash string }{{appFramework, profile.app}, {coreFramework, profile.core}} {
		raw, err := r.run("cat /opt/hmi/ID5/lib/"+framework.name, nil, 4<<20)
		if err != nil {
			result.Result = "Could not read " + framework.name + " over SSH. Check the head-unit IP and temporary SSH access."
			return result
		}
		if digest(raw) != framework.hash {
			result.Result = "The on-car framework fingerprint for " + framework.name + " does not match the supplied capture. Check the head-unit IP. If it is correct, see the source repository’s compatibility workflow."
			return result
		}
	}
	display, err := r.run("/opt/sys/bin/LayerManagerControl get screen 0", nil, 8192)
	if err == nil {
		result.screen = readPanel(display)
	}
	result.Display = result.screen.String()
	if in.DisplayResolution != "" {
		selected := manualPanel(in.DisplayResolution)
		if !selected.supported() {
			result.Result = "Choose a supported manual resolution. Custom layouts require 800–1920 pixels wide, 480–720 high, and an aspect ratio between 5:3 and 3:1."
			return result
		}
		if result.screen != (panel{}) && result.screen != selected {
			result.Result = "The manual resolution differs from the active iDrive resolution. Choose the detected resolution or use Automatic."
			return result
		}
		result.screen = selected
		result.Display = result.screen.String() + " (manual selection)"
		if !selected.automatic() {
			result.Display += " · Experimental layout"
		}
	} else if !result.screen.automatic() {
		result.Result = "Choose the known iDrive resolution manually, then check compatibility again."
		return result
	}
	coding, _ := r.run(`for f in /var/opt/conn/carplay/carplay.ini /fs/sda1/opt/conn/carplay/carplay.ini; do if test -r "$f"; then sed -n '/^#Coded is:/p' "$f"; break; fi; done`, nil, 1024)
	result.Touch = "Touch coding unknown. Use the iDrive controller to navigate."
	if touchCoding.Match(coding) {
		result.Touch = "Touch coded on this car. Game Room is used with the Xbox pad and the iDrive controller."
	}
	if _, err := usableIP(in.Gateway); err != nil || in.Gateway == in.Head {
		result.Result = "Enter separate head unit and diagnostic gateway ENET addresses."
		return result
	}
	hu, err := gatewayCheck(in.Local, in.Gateway)
	if err != nil {
		result.Result = "Could not reach the diagnostic gateway on port 6801. Check the gateway IP and ENET connection."
		return result
	}
	if hu != "" && hu != in.Head {
		result.Result = "The gateway reports a different head unit IP. Check the ENET connection."
		return result
	}
	if _, err := r.run(writableCheck, nil, 128); err != nil {
		result.Result = "The ID5 folder is not writable, so plugins cannot be installed."
		return result
	}
	result.Result = "Frameworks matched. Ready to install the games (game files go on the hard drive)."
	result.Ready = true
	return result
}

func digest(b []byte) string { sum := sha256.Sum256(b); return hex.EncodeToString(sum[:]) }

type plugin struct{ name, worker, connector string }

// apps are what this installer handles: Game Room. Other plugins on the car are never touched. Each game's
// data files go on the hard drive (games.go); only the small plugin goes in Plugins/ on the flash.
var apps = []plugin{
	// Game Room (2026-11-20): one entry for Freedoom (doomgeneric, GPL-2.0, with Freedoom's WADs, BSD-3-Clause),
	// Tyrian (OpenTyrian, GPL-2.0, with the freeware Tyrian 2.1 data) and Quake (quakegeneric, GPL-2.0, with
	// LibreQuake's data). freedoom-qnx shows the combined list and becomes tyrian-qnx or quake-qnx for theirs.
	// Since 2026-11-20 evening also the console games ("just have everything in Game Room"): NES (FCEUmm), Game Boy
	// / Color (Gambatte) and PlayStation (PCSX ReARMed), all GPL-2.0, with freely licensed homebrew.
	{"GameRoom", "freedoom-qnx", "220"},
}

// appPayloadExtras are files an app ships besides the usual six (manifest, layout, images, texts, library, worker).
// Game data files go on the hard drive (gameFiles in games.go); the only extras are second programs:
// Retro's Game Boy emulator, which the NES one hands over to (and back) for .gb/.gbc games.
func appPayloadExtras(name string) []string {
	switch name {
	case "GameRoom":
		// PS1 candidate: QNX uses the existing bytewise register-map lookup in
		// its dynarec. This addresses the captured compiler fault, not every crash.
		return []string{"tyrian-qnx", "quake-qnx", "nes-qnx", "gb-qnx", "ps1-qnx", "psp-qnx", "ppsspp_libretro.so"}
	}
	return nil
}

// oldGamePlugins are the separate game entries Game Room replaced (2026-11-20). Install removes their plugin
// folders first (their connectors 220-222 would otherwise clash), keeping their game files on the hard drive
// (Game Room uses the same folders), the owner's own games and every save.
var oldGamePlugins = []plugin{{"Freedoom", "freedoom-qnx", "220"}, {"Tyrian", "tyrian-qnx", "221"}, {"LibreQuake", "quake-qnx", "222"}, {"Retro", "nes-qnx", "223"}}

// presentOldGamePlugins lists which of them are on the car.
func presentOldGamePlugins(r remoteRunner) ([]plugin, error) {
	var found []plugin
	for _, p := range oldGamePlugins {
		path := root + "/Plugins/" + p.name
		out, err := r.run("if test -e "+path+" || test -L "+path+"; then echo present; fi", nil, 128)
		if err != nil {
			return nil, err
		}
		if strings.TrimSpace(string(out)) == "present" {
			found = append(found, p)
		}
	}
	return found, nil
}

func isAppExtraFile(app, name string) bool {
	for _, extra := range appPayloadExtras(app) {
		if extra == name {
			return true
		}
	}
	return false
}

// legacyAppNames are Game Room's own earlier plugin folders: the separate game entries it replaced
// (Uninstall everything removes them too; install replaces them itself, see oldGamePlugins).
var legacyAppNames = []string{"Freedoom", "Tyrian", "LibreQuake", "Retro"}

// presentLegacyApps lists which legacy plugin folders still exist on the car.
func presentLegacyApps(r remoteRunner) ([]string, error) {
	var found []string
	for _, name := range legacyAppNames {
		path := root + "/Plugins/" + name
		out, err := r.run("if test -e "+path+" || test -L "+path+"; then echo present; fi", nil, 128)
		if err != nil {
			return nil, err
		}
		if strings.TrimSpace(string(out)) == "present" {
			found = append(found, name)
		}
	}
	return found, nil
}

type manifest struct {
	Name      string `xml:"name"`
	Connector string `xml:"config>dependencies>pluginAppConnector>id"`
}

var pluginPath = regexp.MustCompile(`^/var/opt/hmi/data/ID5/Plugins/[A-Za-z0-9_.+/-]+\.manifest$`)

func checkConnectors(r remoteRunner) error {
	list, err := r.run("find "+root+"/Plugins -name '*.manifest' -type f", nil, 32768)
	if err != nil {
		return fmt.Errorf("could not inspect existing plugins: %w", err)
	}
	for _, path := range strings.Fields(string(list)) {
		if !pluginPath.MatchString(path) || strings.Contains(path, "..") {
			return errors.New("unexpected existing plugin path")
		}
		raw, err := r.run("cat "+path, nil, 65536)
		if err != nil {
			return err
		}
		var m manifest
		if err = xml.Unmarshal(raw, &m); err != nil {
			return fmt.Errorf("could not validate existing manifest %s", path)
		}
		for _, app := range apps {
			if strings.TrimSpace(m.Connector) == app.connector && (m.Name != app.name || path != root+"/Plugins/"+app.name+"/"+app.name+".manifest") {
				return fmt.Errorf("connector %s is already used by %s", app.connector, path)
			}
		}
	}
	return nil
}

// selectedApps filters the known app list down to the names the user
// checked in the UI. An empty selection means "all apps" (keeps old
// clients and the recover flow, which never sends Apps, working as before).
func selectedApps(names []string) ([]plugin, error) {
	if len(names) == 0 {
		return append([]plugin{}, apps...), nil
	}
	wanted := make(map[string]bool, len(names))
	for _, name := range names {
		wanted[name] = true
	}
	var selected []plugin
	for _, app := range apps {
		if wanted[app.name] {
			selected = append(selected, app)
			delete(wanted, app.name)
		}
	}
	if len(wanted) != 0 {
		return nil, errors.New("the selection included an unknown app")
	}
	if len(selected) == 0 {
		return nil, errors.New("choose at least one app to install")
	}
	return selected, nil
}

type blockedInstall struct {
	report         string
	canRecover     bool
	canUnlockEmpty bool
}

func (e *blockedInstall) Error() string {
	return "The install lock already exists.\n\n" + e.report
}

// The lock remains held through staging, publication and final readback.
func claimLock(r remoteRunner) error {
	_, err := r.run("mkdir "+installLock, nil, 512)
	return err
}
func releaseLock(r remoteRunner) {
	_, _ = r.run(`lock=`+installLock+`
if test ! -e "$lock" && test ! -L "$lock"; then exit 0; fi
test -d "$lock" && test ! -L "$lock" || exit 1
for p in "$lock"/* "$lock"/.[!.]* "$lock"/..?*; do
    if test -e "$p" || test -L "$p"; then exit 1; fi
done
rm -r "$lock"`, nil, 512)
}
func runMutation(r remoteRunner, command string, input []byte, limit int) ([]byte, error) {
	if long, ok := r.(interface {
		runLong(string, []byte, int) ([]byte, error)
	}); ok {
		return long.runLong(command, input, limit)
	}
	return r.run(command, input, limit)
}

// noBackupApps keep no backups at all (owner, 2026-11-20): Freedoom's two game files are 57 MB of data that
// never changes, on the car's ~492 MiB /var flash partition (sda0t178) shared with the rest of /var. During
// publication the old copy is still parked as old-<App> so a failed publish can roll back; it is deleted once
// the install is verified, and every older one is deleted before staging starts.
var noBackupApps = []string{"GameRoom", "Freedoom", "Tyrian", "LibreQuake", "Retro"}

// noBackupPurge deletes every backup of the noBackupApps from finished transactions (those with a complete
// marker; in-progress ones are never touched), then removes transactions left holding no backup at all.
func noBackupPurge(skip string) string {
	var body strings.Builder
	body.WriteString("for d in " + root + "/AppInstaller-Backup-????????????????; do\n")
	body.WriteString("test \"$d\" = \"" + skip + "\" && continue\n")
	body.WriteString(`test -d "$d" && test ! -L "$d" && test -f "$d/complete" || continue
`)
	for _, name := range noBackupApps {
		fmt.Fprintf(&body, "rm -rf \"$d/old-%[1]s\" \"$d/removed-%[1]s\" \"$d/settings-%[1]s\"\n", name)
	}
	body.WriteString(`keep=0
for p in "$d"/old-* "$d"/removed-* "$d"/settings-*; do
    if test -e "$p" || test -L "$p"; then keep=1; fi
done
if test "$keep" = 0; then rm -rf "$d"; fi
done
`)
	return body.String()
}

// Retention only prunes an older app backup when this transaction replaces it.
// In-progress folders have no complete marker and are never cleanup candidates.
// The noBackupApps' copies in this transaction go too (the install or uninstall has finished by now).
func backupRetentionScript(tx string, selected []plugin) []byte {
	if len(selected) == 0 {
		selected = apps
	}
	var body strings.Builder
	fmt.Fprintf(&body, "tx=%s\n: > \"$tx/complete\" || exit 1\n", tx)
	body.WriteString("for d in " + root + "/AppInstaller-Backup-????????????????; do\n")
	body.WriteString(`test "$d" = "$tx" && continue
test -d "$d" && test ! -L "$d" && test -f "$d/complete" || continue
`)
	for _, app := range selected {
		fmt.Fprintf(&body, `if test -d "$tx/old-%[1]s" || test -d "$tx/removed-%[1]s"; then
    rm -rf "$d/old-%[1]s" "$d/removed-%[1]s" "$d/settings-%[1]s"
fi
`, app.name)
	}
	body.WriteString(`keep=0
for p in "$d"/old-* "$d"/removed-* "$d"/settings-*; do
    if test -e "$p" || test -L "$p"; then keep=1; fi
done
if test "$keep" = 0; then rm -rf "$d"; fi
done
`)
	for _, name := range noBackupApps {
		fmt.Fprintf(&body, "rm -rf \"$tx/old-%[1]s\" \"$tx/removed-%[1]s\" \"$tx/settings-%[1]s\"\n", name)
	}
	body.WriteString(noBackupPurge(tx))
	return []byte(body.String())
}

func pruneBackups(r remoteRunner, tx string, selected []plugin) {
	_, _ = runMutation(r, "/bin/sh -s", backupRetentionScript(tx, selected), 2048)
}

func install(r remoteRunner, gateway string, screen panel, recoverLock bool, selected []plugin) (string, error) {
	if _, err := usableIP(gateway); err != nil {
		return "", err
	}
	if !screen.supported() {
		return "", errors.New("no supported display layout was selected")
	}
	lockCheck := "if test -e " + installLock + " || test -L " + installLock + "; then echo locked; fi"
	lock, err := r.run(lockCheck, nil, 512)
	if err != nil {
		return "", fmt.Errorf("install lock check failed: %w", err)
	}
	locked := strings.TrimSpace(string(lock)) == "locked"
	if locked {
		state, report, err := inspectInstallLock(r, "")
		if err != nil {
			return "", fmt.Errorf("could not inspect the existing install lock: %w", err)
		}
		if !recoverLock || state != "staged-only" {
			canUnlock, _ := emptyInstallLock(r)
			return "", &blockedInstall{report: report + "\nEmpty-lock release v2: " + fmt.Sprint(canUnlock), canRecover: state == "staged-only", canUnlockEmpty: canUnlock}
		}
	} else if len(bytes.TrimSpace(lock)) != 0 {
		return "", errors.New("unexpected response from the install-lock check")
	}
	releaseOwned := false
	defer func() {
		if releaseOwned {
			releaseLock(r)
		}
	}()
	if !locked {
		if err := claimLock(r); err != nil {
			return "", fmt.Errorf("install lock: %w", err)
		}
		releaseOwned = true
	}
	// A car that has never had any HMI plugin installed simply doesn't have
	// this folder yet; create it (ID5 itself must already be writable, per
	// writableCheck) before checkConnectors or anything else looks inside it.
	if _, err := r.run("test -d "+root+"/Plugins || { umask 022 && mkdir "+root+"/Plugins; }", nil, 512); err != nil {
		return "", fmt.Errorf("could not create the Plugins folder: %w", err)
	}
	if err := checkConnectors(r); err != nil {
		return "", err
	}
	var nonce [8]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return "", err
	}
	tx := root + "/" + backupPrefix + hex.EncodeToString(nonce[:])
	if _, err := r.run("umask 077; mkdir "+tx+" && chmod 755 "+tx, nil, 512); err != nil {
		return "", err
	}
	if err := stageFileOps(r, tx); err != nil {
		return tx, err
	}
	if locked {
		state, report, err := inspectInstallLock(r, tx)
		if err != nil {
			return tx, fmt.Errorf("could not recover the existing install lock: %w", err)
		}
		if state != "cleared" {
			return tx, &blockedInstall{report: report}
		}
		if err := claimLock(r); err != nil {
			return tx, fmt.Errorf("install lock: %w", err)
		}
		releaseOwned = true
	}
	gameNotes = nil
	if markers := rebuildMarkerReport(r); markers != "" {
		gameNote("NOTE: the car has rebuilt a hard-drive partition after a fault at some point (%s). Game files there may have been removed; this install puts them back.", strings.ReplaceAll(markers, "\n", ", "))
	}
	// Saves an earlier uninstall moved into a backup go back first; then the old game backups can go.
	switch saves, err := rescueFreedoomSaves(r, tx); {
	case err != nil:
		return tx, fmt.Errorf("could not check for Freedoom saves in old backups: %w", err)
	case strings.HasPrefix(saves, "restored"):
		gameNote("Freedoom saves were restored from an old uninstall backup.")
	}
	// Free the space held by backups of the apps that keep none before uploading anything (best-effort).
	_, _ = runMutation(r, "/bin/sh -s", []byte(noBackupPurge(tx)), 2048)
	verifyFiles := make(map[string][]byte)
	for _, app := range selected {
		stage := tx + "/new-" + app.name
		live := root + "/Plugins/" + app.name
		// Treat an empty leftover folder as absent. The helper uses rmdir, so a file
		// appearing after the emptiness check cannot be recursively deleted.
		if _, err := r.run(emptyDirRemoval(live, tx+"/"+fileOpsName), nil, 512); err != nil {
			return tx, fmt.Errorf("could not clear the empty %s folder: %w", app.name, err)
		}
		check := "test ! -L " + live + " && { test ! -e " + live + " || { test -d " + live + " && test -f " + live + "/" + app.name + ".manifest; }; }"
		if _, err := r.run(check+" && umask 022 && mkdir "+stage, nil, 512); err != nil {
			return tx, fmt.Errorf("could not prepare %s staging directory: %w", app.name, err)
		}
		files, err := fs.ReadDir(bundled, "payload/"+app.name)
		if err != nil {
			return tx, err
		}
		filesData := map[string][]byte{"gateway-ip.txt": []byte(gateway + "\n")}
		for _, file := range files {
			name := file.Name()
			switch name {
			case app.name + ".manifest", app.name + ".xml", app.name + "_ALL_Images.zip", app.name + "_ALL_Texts.zip", "lib" + app.name + ".so", app.worker:
			default:
				if !isAppExtraFile(app.name, name) {
					return tx, errors.New("unexpected file in embedded payload")
				}
			}
			data, err := fs.ReadFile(bundled, "payload/"+app.name+"/"+name)
			if err != nil {
				return tx, err
			}
			if name == app.name+".xml" {
				data = scaleLayout(data, app.name, screen)
			}
			filesData[name] = data
		}
		if len(filesData) != 7+len(appPayloadExtras(app.name)) {
			return tx, errors.New("embedded payload is incomplete")
		}
		for name, data := range filesData {
			path := stage + "/" + name
			if err := uploadFile(r, tx+"/"+fileOpsName, path, data); err != nil {
				if errors.Is(err, errUploadVerify) {
					return tx, fmt.Errorf("upload verification failed for %s", name)
				}
				return tx, fmt.Errorf("could not upload %s: %w", name, err)
			}
			verifyFiles[live+"/"+name] = data
		}
		for _, program := range append([]string{app.worker}, appPayloadExtras(app.name)...) {
			if !strings.HasSuffix(program, "-qnx") {
				continue
			}
			if _, err := r.run("chmod 755 "+stage+"/"+program, nil, 512); err != nil {
				return tx, fmt.Errorf("could not set execute permission on %s: %w", program, err)
			}
		}
	}
	// The game files go on the hard drive before the plugin is published, so a game never appears in
	// My Vehicle without its files.
	for _, app := range selected {
		for _, dir := range gameDataDirs(app.name) {
			if err := installGameData(r, tx, dir); err != nil {
				return tx, err
			}
		}
	}
	// Keep the lock and rollback in one QNX shell. Pass the script over stdin
	// so SSH only has to execute a short, fixed command.
	releaseOwned = false // The shell retains the lock if rollback fails or SSH is lost.
	if _, err := runMutation(r, "/bin/sh -s", publicationScript(tx, selected, true), 1024); err != nil {
		return tx, fmt.Errorf("publication was interrupted: %w", err)
	}
	releaseOwned = true
	for path, expected := range verifyFiles {
		if len(expected) > uploadChunk {
			if err := checkRemoteSize(r, tx+"/"+fileOpsName, path, len(expected)); err != nil {
				return tx, fmt.Errorf("could not verify published file %s: %w", path, err)
			}
			continue
		}
		actual, err := r.run("cat "+path, nil, len(expected)+1)
		if err != nil {
			return tx, fmt.Errorf("could not read back published file %s: %w", path, err)
		}
		if !bytes.Equal(actual, expected) {
			return tx, fmt.Errorf("published file differs from staged data: %s", path)
		}
	}
	// Every successful install/upgrade leaves its previous version behind as
	// old-<App> inside this transaction's own folder, for manual recovery.
	// Without this, repeated installs accumulate one of these per run
	// indefinitely. Keep only the one just created; this is best-effort and
	// never fails the install itself if cleanup doesn't fully succeed.
	pruneBackups(r, tx, selected)
	return tx, nil
}

// uploadChunk: every SSH command has an 8-second budget (runDeadline). freedoom1.wad (28 MB) in one command
// overran it on the car (2026-11-19: "remote command exited without exit status or exit signal"), which also
// killed the connection before the install lock could be released. Files above this size go up in pieces:
// each piece is written to path.part, read back and compared, then appended; the final size is checked with
// the staged helper (the car's shell has no ls or wc).
var uploadChunk = 256 * 1024

var errUploadVerify = errors.New("upload verification failed")

func uploadFile(r remoteRunner, ops, path string, data []byte) error {
	if len(data) <= uploadChunk {
		if _, err := r.run("umask 022; set -C; cat > "+path, data, 512); err != nil {
			return err
		}
		read, err := r.run("cat "+path, nil, len(data)+1)
		if err != nil || !bytes.Equal(read, data) {
			return errUploadVerify
		}
		return nil
	}
	part := path + ".part"
	if _, err := r.run("umask 022; set -C; : > "+path, nil, 512); err != nil {
		return err
	}
	for start := 0; start < len(data); start += uploadChunk {
		end := start + uploadChunk
		if end > len(data) {
			end = len(data)
		}
		piece := data[start:end]
		if _, err := r.run("rm -f "+part+"; umask 022; set -C; cat > "+part, piece, 512); err != nil {
			return fmt.Errorf("piece at %d: %w", start, err)
		}
		read, err := r.run("cat "+part, nil, len(piece)+1)
		if err != nil || !bytes.Equal(read, piece) {
			return errUploadVerify
		}
		if _, err := r.run("cat "+part+" >> "+path+" && rm -f "+part, nil, 512); err != nil {
			return fmt.Errorf("piece at %d: %w", start, err)
		}
	}
	if err := checkRemoteSize(r, ops, path, len(data)); err != nil {
		return fmt.Errorf("%w: %v", errUploadVerify, err)
	}
	return nil
}

// checkRemoteSize asks the staged helper (gameroom-fileops size) for a file's byte count.
func checkRemoteSize(r remoteRunner, ops, path string, want int) error {
	script := "ops=" + ops + "\n\"$ops\" size " + path + "\n"
	out, err := r.run("/bin/sh -s", []byte(script), 64)
	if err != nil {
		return err
	}
	got, err := strconv.Atoi(strings.TrimSpace(string(out)))
	if err != nil || got != want {
		return fmt.Errorf("size is %q, expected %d", strings.TrimSpace(string(out)), want)
	}
	return nil
}

func stageFileOps(r remoteRunner, tx string) error {
	data, err := fs.ReadFile(bundled, "payload/tools/"+fileOpsName)
	if err != nil {
		return err
	}
	path := tx + "/" + fileOpsName
	if _, err := r.run("umask 077; set -C; cat > "+path, data, 512); err != nil {
		return fmt.Errorf("could not upload the file operations helper: %w", err)
	}
	actual, err := r.run("cat "+path, nil, len(data)+1)
	if err != nil {
		return fmt.Errorf("could not read back the file operations helper: %w", err)
	}
	if !bytes.Equal(actual, data) {
		return errors.New("file operations helper differs from the bundled copy")
	}
	if _, err := r.run("chmod 700 "+path+" && "+path+" check", nil, 512); err != nil {
		return fmt.Errorf("the head unit could not run the bundled file operations helper: %w", err)
	}
	return nil
}

func inspectInstallLock(r remoteRunner, recoveryTx string) (string, string, error) {
	var script bytes.Buffer
	fmt.Fprintf(&script, "root=%s\nlock=%s\ncurrent=%s\nops=\"$current/%s\"\napps='", root, installLock, recoveryTx, fileOpsName)
	for _, app := range apps {
		fmt.Fprintf(&script, "%s ", app.name)
	}
	script.WriteString(`'
exists() { test -e "$1" || test -L "$1"; }
lock_empty() {
    for member in "$lock"/* "$lock"/.[!.]* "$lock"/..?*; do
        if exists "$member"; then return 1; fi
    done
    return 0
}
staged_only() {
    test -d "$lock" && test ! -L "$lock" && lock_empty || return 1
    for app in $apps; do
        if exists "$root/Plugins/$app"; then return 1; fi
    done
    complete=0
    for tx in "$root"/AppInstaller-Backup-????????????????; do
        if test "$tx" = "$current" || ! exists "$tx"; then continue; fi
        test -d "$tx" && test ! -L "$tx" || return 1
        for marker in "$tx"/removed-* "$tx"/settings-*; do
            if exists "$marker"; then return 1; fi
        done
        has_all_stages=1
        for app in $apps; do
            if exists "$tx/old-$app" || exists "$tx/fresh-$app"; then return 1; fi
            if ! test -d "$tx/new-$app" || test -L "$tx/new-$app"; then has_all_stages=0; fi
        done
        if test "$has_all_stages" = 1; then complete=1; fi
    done
    test "$complete" = 1
}
# A lock left by an install whose STAGING failed (2026-11-19: the connection dropped while uploading
# freedoom1.wad, so the lock could not be released). The newest transaction is the one that held the lock;
# if it holds only new-* staging folders and none of the publication markers (old-, fresh-, removed-,
# settings-), nothing live was touched and the lock can go. Uses the shell's own -nt (no ls on the car).
failed_staging() {
    test -d "$lock" && test ! -L "$lock" && lock_empty || return 1
    newest=
    for tx in "$root"/AppInstaller-Backup-????????????????; do
        if test "$tx" = "$current" || ! exists "$tx"; then continue; fi
        if test -z "$newest" || test "$tx" -nt "$newest"; then newest=$tx; fi
    done
    test -n "$newest" && test -d "$newest" && test ! -L "$newest" || return 1
    for marker in "$newest"/old-* "$newest"/fresh-* "$newest"/removed-* "$newest"/settings-*; do
        if exists "$marker"; then return 1; fi
    done
    for stage in "$newest"/new-*; do
        if exists "$stage"; then return 0; fi
    done
    return 1
}
# An upload can remove its temporary helper folder, then lose SSH before releasing
# the empty lock. Only explicit recovery may clear it: every live app must still
# have its manifest and every retained app transaction must be complete.
# Empty alone is never sufficient. The UI asks the owner to close other installers.
orphan_empty() {
    test -d "$lock" && test ! -L "$lock" && lock_empty || return 1
    for app in $apps; do
        live="$root/Plugins/$app"
        test -d "$live" && test ! -L "$live" || return 1
        test -f "$live/$app.manifest" && test ! -L "$live/$app.manifest" || return 1
    done
    for tx in "$root"/AppInstaller-Backup-????????????????; do
        if test "$tx" = "$current" || ! exists "$tx"; then continue; fi
        test -d "$tx" && test ! -L "$tx" || return 1
        test -f "$tx/complete" && test ! -L "$tx/complete" || return 1
    done
    return 0
}
if staged_only || failed_staging || orphan_empty; then
    if test -n "$current"; then
        "$ops" unlock "$lock" || exit 1
        echo cleared
        exit 0
    fi
    echo staged-only
else
    echo review
fi
show_path() {
    if test -L "$1"; then state=symlink
    elif test -d "$1"; then state=directory
    elif test -f "$1"; then state=file
    elif test -e "$1"; then state=other
    else state=missing
    fi
    echo "$2: $state"
}
echo "Install lock: $lock"
show_path "$lock" 'Lock type'
if test -d "$lock" && test ! -L "$lock"; then
    if lock_empty; then echo 'Lock contents: empty'; else echo 'Lock contents: nonempty'; fi
fi
echo
echo 'Live app paths:'
for app in $apps; do
    show_path "$root/Plugins/$app" "$app"
    show_path "$root/Plugins/$app/$app.manifest" "$app manifest"
done
for tx in "$root"/AppInstaller-Backup-????????????????; do
    if test ! -d "$tx" || test -L "$tx"; then continue; fi
    echo
    echo "Transaction $tx"
    for app in $apps; do
        for prefix in new old fresh removed settings; do
            marker="$prefix-$app"
            show_path "$tx/$marker" "$marker"
        done
    done
done
`)
	out, err := r.run("/bin/sh -s", script.Bytes(), 16384)
	if err != nil {
		return "", "", err
	}
	state, report, _ := strings.Cut(string(out), "\n")
	switch state {
	case "cleared", "staged-only", "review":
		return state, strings.TrimSpace(report), nil
	default:
		return "", "", errors.New("unexpected install recovery response")
	}
}

func publicationScript(tx string, selected []plugin, held ...bool) []byte {
	var script bytes.Buffer
	names := make([]string, 0, len(selected))
	for _, app := range selected {
		names = append(names, app.name+":"+app.worker)
	}
	fmt.Fprintf(&script, "root=%s\ntx=%s\nops=\"$tx/%s\"\napps='%s'\n", root, tx, fileOpsName, strings.Join(names, " "))
	prelocked := 0
	if len(held) > 0 && held[0] {
		prelocked = 1
	}
	fmt.Fprintf(&script, "prelocked=%d\n", prelocked)
	script.WriteString(`lock="$root/ID6-install-lock"
fail() { echo "$1" >&2; exit 1; }
paths() {
    live="$root/Plugins/$app"
    old="$tx/old-$app"
    stage="$tx/new-$app"
    fresh="$tx/fresh-$app"
}
# Freedoom keeps its saves and settings in $root/Freedoom-data (from 2026-11-20), outside the plugin folder
# that every install replaces. Older builds wrote them inside the plugin folder: they are moved out before the
# folder is replaced, never over ones already there. Engine names: .savegame/ (doomsavN.dsg), default.cfg,
# doomgenericdoom.cfg.
keep_freedoom_data() {
    data="$root/Freedoom-data"
    test ! -L "$data" || fail 'Freedoom-data is a symbolic link.'
    if ! test -d "$data"; then mkdir "$data" || fail 'Could not create Freedoom-data.'; fi
    for item in .savegame default.cfg doomgenericdoom.cfg; do
        if test -e "$live/$item" && ! test -L "$live/$item" && ! test -e "$data/$item"; then
            "$ops" move "$live/$item" "$data/$item" || fail "Could not keep Freedoom's $item."
        fi
    done
}
cleanup() {
    if test "$locked" = 0; then return; fi
    rollback_failed=0
    if test "$done_ok" = 0; then
        for entry in $apps; do
	            app=${entry%%:*}
            paths
            if test -d "$old" || test -f "$fresh"; then
                if test ! -e "$stage" && test -d "$live"; then
                    "$ops" move "$live" "$stage" || rollback_failed=1
                fi
                if test -d "$old"; then
                    if test -e "$live" || test -L "$live"; then
                        rollback_failed=1
                    else
                        "$ops" move "$old" "$live" || rollback_failed=1
                    fi
                fi
            fi
        done
    fi
    if test "$rollback_failed" = 1; then
        echo 'Rollback needs inspection. The install lock and backups were kept.' >&2
    else
        if test "$prelocked" = 1 && test "$done_ok" = 1; then return; fi
        if ! "$ops" unlock "$lock"; then
            trap - 0
            fail 'Could not release the install lock.'
        fi
    fi
}
locked=0
done_ok=0
trap cleanup 0
trap 'fail "Publication received a stop signal."' HUP INT TERM
if test "$prelocked" != 1 && ! mkdir "$lock"; then
    if test -e "$lock" || test -L "$lock"; then
        fail 'The install lock already exists. Check for an active or interrupted install before retrying.'
    fi
    fail 'Could not create the install lock.'
fi
locked=1
for entry in $apps; do
    app=${entry%%:*}
    worker=${entry#*:}
    paths
    test ! -L "$live" || fail "$app plugin path is a symbolic link."
    if test -e "$live"; then
        test -d "$live" && test -f "$live/$app.manifest" || fail "$app plugin path cannot be replaced."
        if test "$app" = Freedoom; then keep_freedoom_data; fi
        "$ops" move "$live" "$old" || fail "Could not back up $app."
    else
        : > "$fresh" || fail "Could not record the new $app install."
    fi
    "$ops" move "$stage" "$live" || fail "Could not publish $app."
    test -f "$live/$app.manifest" && test -x "$live/$worker" || fail "Published $app is missing its manifest or executable worker."
done
rm -rf /var/opt/hmi/data/ID5/PSP-data/PSP/SYSTEM/CACHE/* /tmp/*.glshadercache /tmp/ppsspp* 2>/dev/null || true
done_ok=1
`)
	return script.Bytes()
}

// uninstall removes the live plugin directories of selected (or, if empty,
// every) app from the car, keeping a backup of whatever it removed in a
// fresh transaction directory (nothing is deleted outright, matching the
// install path's recoverability). An empty selected also removes the
// legacy pre-rename folders and every app's own stray file outside its
// plugin folder (gateway-ip.txt lives inside each app's own plugin folder
// already, so moving that folder out takes it with it); a non-empty
// selected touches only those apps' own folder and stray files, leaving
// the rest of the install untouched.
func uninstall(r remoteRunner, selected []plugin) (string, []string, error) {
	lockCheck := "if test -e " + installLock + " || test -L " + installLock + "; then echo locked; fi"
	lock, err := r.run(lockCheck, nil, 512)
	if err != nil {
		return "", nil, fmt.Errorf("install lock check failed: %w", err)
	}
	if strings.TrimSpace(string(lock)) == "locked" {
		_, report, err := inspectInstallLock(r, "")
		if err != nil {
			return "", nil, fmt.Errorf("could not inspect the existing install lock: %w", err)
		}
		return "", nil, fmt.Errorf("an install is already in progress or left an unresolved lock.\n\n%s", report)
	} else if len(bytes.TrimSpace(lock)) != 0 {
		return "", nil, errors.New("unexpected response from the install-lock check")
	}
	if err := claimLock(r); err != nil {
		return "", nil, err
	}
	releaseOwned := true
	defer func() {
		if releaseOwned {
			releaseLock(r)
		}
	}()
	var nonce [8]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return "", nil, err
	}
	tx := root + "/" + backupPrefix + hex.EncodeToString(nonce[:])
	if _, err := r.run("umask 077; mkdir "+tx+" && chmod 755 "+tx, nil, 512); err != nil {
		return "", nil, err
	}
	if err := stageFileOps(r, tx); err != nil {
		return tx, nil, err
	}
	releaseOwned = false // A failed or interrupted script owns rollback and lock retention.
	out, err := runMutation(r, "/bin/sh -s", uninstallScript(tx, selected, true), 4096)
	if err != nil {
		return tx, strings.Fields(string(out)), fmt.Errorf("uninstall interrupted; inspect %s for the outcome and any rollback: %w", tx, err)
	}
	// Verification, retention and unlock completed in that same remote shell.
	// There must be no follow-up SSH call needed to finish a successful removal.
	return tx, strings.Fields(string(out)), nil
}

// uninstallNames is every plugin folder Uninstall removes for this
// selection: the selected apps alone, or, if selected is empty, every
// current app plus the legacy folder names from before the rename.
func uninstallNames(selected []plugin) []string {
	if len(selected) == 0 {
		names := make([]string, 0, len(apps)+len(legacyAppNames))
		for _, app := range apps {
			names = append(names, app.name)
		}
		return append(names, legacyAppNames...)
	}
	names := make([]string, len(selected))
	for i, app := range selected {
		names[i] = app.name
	}
	return names
}

// appExtraFiles are the files each app keeps outside its own plugin folder
// (so moving that folder out, as uninstallScript does below, does not take
// them with it) - removed only when that app itself is being uninstalled.
var appExtraFiles = map[string][]string{} // games keep their saves in place (Freedoom-data) on uninstall

func uninstallScript(tx string, selected []plugin, held ...bool) []byte {
	clean := selected
	if len(clean) == 0 {
		clean = apps
	}
	var script bytes.Buffer
	prelocked := 0
	if len(held) > 0 && held[0] {
		prelocked = 1
	}
	fmt.Fprintf(&script, "root=%s\ntx=%s\nops=\"$tx/%s\"\napps='%s'\nprelocked=%d\n", root, tx, fileOpsName, strings.Join(uninstallNames(selected), " "), prelocked)
	script.WriteString(`lock="$root/ID6-install-lock"
fail() { echo "$1" >&2; exit 1; }
cleanup() {
    test "$locked" = 1 || return
    failed=0
    if test "$done_ok" != 1; then
        for app in $apps; do
            if test -d "$tx/removed-$app"; then
                "$ops" move "$tx/removed-$app" "$root/Plugins/$app" || failed=1
            fi
            if test -d "$tx/settings-$app"; then
                for file in "$tx/settings-$app"/*; do
                    test -f "$file" || test -d "$file" || continue
                    "$ops" move "$file" "$root/${file##*/}" || failed=1
                done
            fi
        done
    fi
    if test "$failed" = 1; then
        echo 'Rollback incomplete; lock and backups retained.' >&2
    else
        if ! "$ops" unlock "$lock"; then
            trap - 0
            fail 'Could not release the uninstall lock; retained for inspection.'
        fi
    fi
}
locked=0
done_ok=0
trap cleanup 0
trap 'fail "Uninstall received a stop signal."' HUP INT TERM
if test "$prelocked" != 1 && ! mkdir "$lock"; then fail 'The install lock already exists.'; fi
locked=1
for app in $apps; do
    live="$root/Plugins/$app"
    test ! -L "$live" || fail "$app plugin path is a symbolic link."
    if test -e "$live"; then
        test -d "$live" || fail "$app plugin path is not a directory."
        "$ops" move "$live" "$tx/removed-$app" || fail "Could not remove $app."
        echo "$app"
    fi
done
`)
	for _, app := range clean {
		fmt.Fprintf(&script, "mkdir \"$tx/settings-%s\" || fail 'Could not back up settings.'\n", app.name)
		for _, name := range appExtraFiles[app.name] {
			fmt.Fprintf(&script, `if test -e "$root/%[1]s" || test -L "$root/%[1]s"; then
    test ! -L "$root/%[1]s" || fail 'Settings path is a symlink.'
    "$ops" move "$root/%[1]s" "$tx/settings-%[2]s/%[1]s" || fail 'Could not back up settings.'
fi
`, name, app.name)
		}
	}
	script.WriteString(`for app in $apps; do
    if test -e "$root/Plugins/$app" || test -L "$root/Plugins/$app"; then
        fail "$app is still present after uninstall."
    fi
done
done_ok=1
(
`)
	script.Write(backupRetentionScript(tx, selected))
	script.WriteString(") || echo 'Backup cleanup did not complete; existing backups retained where possible.' >&2\n")
	return script.Bytes()
}

// displayName maps an app's internal folder name (kept stable so upgrades
// replace the existing install) to the name shown in iDrive and the installer.
func displayName(name string) string {
	switch name {
	case "GameRoom":
		return "Game Room (all the games: Freedoom, Tyrian, Quake, NES, Game Boy, PlayStation, PSP preview; wired Xbox pad)"
	case "Freedoom":
		return "Freedoom (old separate entry)"
	case "Tyrian":
		return "Tyrian (old separate entry)"
	case "LibreQuake":
		return "LibreQuake (old separate entry)"
	case "Retro":
		return "Retro (old separate entry)"
	}
	return name
}

func displayNames(names []string) []string {
	out := make([]string, len(names))
	for i, name := range names {
		out[i] = displayName(name)
	}
	return out
}

// emptyDirRemoval leaves nonempty directories alone. The staged helper's unlock
// command calls rmdir atomically; never replace it with recursive shell removal.
func emptyDirRemoval(dir, ops string) string {
	return "d=" + shellQuote(dir) + "\nops=" + shellQuote(ops) + `
if test -d "$d" && test ! -L "$d"; then
    for p in "$d"/* "$d"/.[!.]* "$d"/..?*; do
        if test -e "$p" || test -L "$p"; then exit 0; fi
    done
    exec "$ops" unlock "$d"
fi
exit 0`
}
