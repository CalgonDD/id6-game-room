package main

// Pure vehicle-side logic, with no UI toolkit dependency: used first by the
// lxn/walk UI, now by the WebView2 UI in webapp.go. Nothing here changed in
// the redesign - only how it's called did.

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"golang.org/x/crypto/ssh"
)

// longRunner is what the diagnostics steps need: a short and a long command.
type longRunner interface {
	remoteRunner
	runLong(cmd string, input []byte, limit int) ([]byte, error)
}

// carLink is the SSH connection a diagnostics pull uses. A step that times out leaves the
// socket's deadline expired and the SSH transport unusable, so without this every later step
// failed at once ("i/o timeout") - in practice losing the crash dump and logs behind one slow
// section. After a step breaks the link, the next step reconnects first; if reconnecting also
// fails (e.g. the head unit is frozen), the remaining steps fail straight away instead of each
// waiting out its own timeouts. A command that merely exits non-zero does not count as broken.
type carLink struct {
	current  linkConn
	dial     func() (linkConn, bool)
	progress func(string)
	broken   bool
	dead     bool
}

// linkConn is one SSH connection (remote on the car, a fake in tests).
type linkConn interface {
	longRunner
	close()
}

func (r remote) close() {
	if r.client != nil {
		r.client.Close()
	}
}

func (l *carLink) run(cmd string, input []byte, limit int) ([]byte, error) {
	return l.do(cmd, input, limit, false)
}

func (l *carLink) runLong(cmd string, input []byte, limit int) ([]byte, error) {
	return l.do(cmd, input, limit, true)
}

func (l *carLink) do(cmd string, input []byte, limit int, long bool) ([]byte, error) {
	if l.dead {
		return nil, errors.New("lost the SSH connection to the head unit and could not reconnect")
	}
	if l.broken {
		if l.progress != nil {
			l.progress("Reconnecting to the head unit…")
		}
		l.current.close()
		next, ok := l.dial()
		if !ok {
			l.dead = true
			return nil, errors.New("lost the SSH connection to the head unit and could not reconnect")
		}
		l.current, l.broken = next, false
	}
	var out []byte
	var err error
	if long {
		out, err = l.current.runLong(cmd, input, limit)
	} else {
		out, err = l.current.run(cmd, input, limit)
	}
	var exit *ssh.ExitError
	if err != nil && !errors.As(err, &exit) {
		l.broken = true
	}
	return out, err
}

func (l *carLink) close() { l.current.close() }

// diagChoice is what the Diagnostics tick boxes asked for.
type diagChoice struct {
	sections  map[string]bool
	crashDump bool
	libraries bool
}

// gatherDiagnostics collects what the tick boxes asked for into one new
// folder on the Desktop (ID6-App-Installer-diagnostics-<time>): report.txt,
// the preferred available crash dump, and the car's libraries under lib\. Read-only on
// the car. progress reports each step to the status box.
func gatherDiagnostics(target longRunner, choice diagChoice, progress func(string)) (summary, report string, err error) {
	stamp := time.Now().Format("2006-01-02_15-04-05")
	home, err := os.UserHomeDir()
	if err != nil {
		return "", "", err
	}
	dir := filepath.Join(home, "Desktop", "ID6-App-Installer-diagnostics-"+stamp)
	if err := os.MkdirAll(filepath.Join(dir, "lib"), 0o755); err != nil {
		return "", "", err
	}
	var lines []string
	// Order matters on a struggling head unit: the crash dump and the whole watchdog logs are the
	// evidence a later crash overwrites, so they come first; the report sections (which re-read
	// state that is still there next time) and the libraries come after.
	if choice.crashDump {
		progress("Looking for a crash dump…")
		dumps, dumpErr := fetchCrashDumps(target)
		if dumpErr != nil {
			lines = append(lines, "Crash dump download failed: "+dumpErr.Error())
		}
		for _, dump := range dumps {
			name, data := dump.name, dump.data
			// The car keeps one core per process until the next crash replaces it, so
			// every pull re-downloads the same file - once counted as several crashes.
			sum := digest(data)
			if when, seen := corePulledBefore(sum); seen {
				lines = append(lines, fmt.Sprintf("The crash dump %s is the same one already pulled on %s (sha256 %s): no new crash since. Not saved again.", name, when, sum[:16]))
			} else {
				if err := os.WriteFile(filepath.Join(dir, name), data, 0o644); err != nil {
					return "", report, fmt.Errorf("could not save crash dump: %w", err)
				}
				rememberCore(sum, stamp, name)
				lines = append(lines, fmt.Sprintf("Saved the crash dump %s (sha256 %s) - a new crash.", name, sum[:16]))
			}
		}
		if len(dumps) == 0 && dumpErr == nil {
			lines = append(lines, "No crash dump on the car.")
		}
	}
	if choice.sections["watchdog"] {
		progress("Downloading the watchdog logs whole…")
		logs, failed, logErr := fetchWatchdogLogs(target)
		if logErr != nil {
			lines = append(lines, "Watchdog logs: "+logErr.Error())
		} else {
			if len(logs) > 0 {
				if err := os.MkdirAll(filepath.Join(dir, "watchdog"), 0o755); err != nil {
					return "", report, err
				}
			}
			for name, data := range logs {
				if err := os.WriteFile(filepath.Join(dir, "watchdog", name), data, 0o644); err != nil {
					return "", report, fmt.Errorf("could not save %s: %w", name, err)
				}
			}
			line := fmt.Sprintf("Saved %d whole watchdog log(s) under watchdog\\.", len(logs))
			if len(failed) > 0 {
				line += " Not downloaded: " + strings.Join(failed, ", ") + "."
			}
			lines = append(lines, line)
		}
	}
	sectionsOn := false
	for _, on := range choice.sections {
		sectionsOn = sectionsOn || on
	}
	if sectionsOn {
		progress("Collecting the selected diagnostics…")
		var pullErr error
		report, pullErr = pullDiagnostics(target, choice.sections)
		// pullDiagnostics returns whatever it collected even when some
		// section didn't finish - keep and save that partial report. The
		// steps before this one may already have saved files, so even an
		// empty report is no reason to throw the folder away.
		if report != "" {
			if err := os.WriteFile(filepath.Join(dir, "report.txt"), []byte(report), 0o644); err != nil {
				return "", report, fmt.Errorf("collected diagnostics but could not save them: %w", err)
			}
		}
		switch {
		case report == "" && pullErr != nil:
			lines = append(lines, "Report not collected: "+pullErr.Error())
		case pullErr != nil:
			lines = append(lines, "Saved a partial report: "+pullErr.Error())
		default:
			lines = append(lines, "Saved the report.")
		}
	}
	if choice.libraries {
		libs, missing := fetchCarLibraries(target, func(done, total int, name string) {
			progress(fmt.Sprintf("Downloading car libraries %d / %d: %s", done+1, total, name))
		})
		for name, data := range libs {
			if err := os.WriteFile(filepath.Join(dir, "lib", name), data, 0o644); err != nil {
				return "", report, fmt.Errorf("could not save %s: %w", name, err)
			}
		}
		line := fmt.Sprintf("Saved %d car libraries.", len(libs))
		if len(missing) > 0 {
			line += " Unavailable or download failed: " + strings.Join(missing, ", ") + "."
		}
		lines = append(lines, line)
	}
	lines = append(lines, copyActionLog(dir))
	summary = "Saved to " + dir + "\n" + strings.Join(lines, "\n")
	// The status box is gone after the next action; keep what this pull found beside its files.
	_ = os.WriteFile(filepath.Join(dir, "pull-summary.txt"), []byte(summary+"\n"), 0o644)
	return summary, report, nil
}

// pulledCoresPath records every crash dump already downloaded (sha256, pull time, name), next to
// the installer's own settings, so the same core pulled again is recognised as an old crash.
func pulledCoresPath() (string, error) {
	settings, err := settingsPath()
	if err != nil {
		return "", err
	}
	return filepath.Join(filepath.Dir(settings), "pulled-cores.txt"), nil
}

func corePulledBefore(sum string) (when string, seen bool) {
	path, err := pulledCoresPath()
	if err != nil {
		return "", false
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return "", false
	}
	for _, line := range strings.Split(string(data), "\n") {
		fields := strings.Fields(line)
		if len(fields) >= 2 && fields[0] == sum {
			return fields[1], true
		}
	}
	return "", false
}

// rememberCore is best effort: failing to record only means a later pull of the same core
// is not flagged as already seen.
func rememberCore(sum, stamp, name string) {
	path, err := pulledCoresPath()
	if err != nil {
		return
	}
	f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o600)
	if err != nil {
		return
	}
	defer f.Close()
	fmt.Fprintf(f, "%s %s %s\n", sum, stamp, name)
}

// doVehicleAction runs entirely off the UI goroutine: it owns the SSH
// connection for one request/response cycle, mirroring what the old local
// HTTP handler did per request.
func doVehicleAction(action string, in vehicleInput, remount bool) inspection {
	target, trust, ok := connectToCar(in)
	if !ok {
		return trust
	}
	defer target.client.Close()
	var remountNote string
	if remount {
		switch mountpoint, err := remountWritable(target); {
		case err != nil:
			remountNote = "Remount: " + err.Error() + "\n"
		case mountpoint != "":
			remountNote = "Remounted " + mountpoint + " read-write.\n"
		}
	}
	if action == "addgames" || action == "listgames" || action == "removegames" || action == "deletestates" {
		result := ownGamesAction(target, action, in)
		result.Result = remountNote + result.Result
		return result
	}
	if action == "deletebackups" {
		removed, err := deleteBackups(target)
		result := inspection{}
		switch {
		case err != nil:
			result.Result = "Delete backups stopped. " + err.Error()
		case len(removed) == 0:
			result.Result = "There were no backups on the car."
		default:
			result.Result = fmt.Sprintf("Deleted %d backup folder(s): %s", len(removed), strings.Join(removed, ", "))
		}
		result.Result = remountNote + result.Result
		return result
	}
	if action == "uninstall" || action == "uninstallselected" {
		var selected []plugin
		result := inspection{}
		if action == "uninstallselected" {
			if len(in.UninstallApps) == 0 {
				// selectedApps treats an empty list as "every app" (install's
				// own "nothing ticked means unchanged" convention) - exactly
				// wrong here, where nothing ticked must mean nothing removed.
				result.Result = remountNote + "Choose at least one app to uninstall."
				return result
			}
			var err error
			selected, err = selectedApps(in.UninstallApps)
			if err != nil {
				result.Result = remountNote + err.Error()
				return result
			}
		}
		gameNotes = nil
		tx, removed, err := uninstall(target, selected)
		if err == nil {
			games := selected
			if len(games) == 0 {
				games = apps
			}
			for _, app := range games {
				for _, dir := range gameDataDirs(app.name) {
					if e := removeGameData(target, dir); e != nil {
						gameNote("Could not remove %s's game files from the hard drive: %v", dir, e)
					}
				}
			}
		}
		switch {
		case err != nil:
			result.Result = "Uninstall stopped. " + err.Error()
			if tx != "" {
				result.Result += " Backups (if any) are at " + tx + "."
			}
		case len(removed) == 0:
			result.Result = "Nothing was installed. There is nothing to remove."
		default:
			result.Result = "Removed: " + strings.Join(displayNames(removed), ", ") + ". Games keep no backups; saves stay on the car. " + restartNote("")
		}
		if len(gameNotes) > 0 {
			result.Result += "\n" + strings.Join(gameNotes, "\n")
		}
		result.Result = remountNote + result.Result
		return result
	}
	checked := inspect(target, in, checkGateway)
	if action == "inspect" {
		defer func() { checked.Result = strings.TrimSpace(checked.Result + "\n" + flashLine(flashStatus(target))) }()
	}
	if action == "unlockempty" {
		if !checked.Ready {
			return checked
		}
		checked.Ready = false
		if err := releaseEmptyInstallLock(target); err != nil {
			checked.Result = "Empty-lock release stopped. " + err.Error()
		} else {
			checked.Result = "Released the empty install lock. Installed apps, games, saves and backup folders were left untouched. Retry Add your games or Install as appropriate."
		}
		return checked
	}
	recoverLock := action == "recover"
	if (recoverLock || action == "install") && checked.Ready {
		// The separate Freedoom, Tyrian and LibreQuake entries make way for Game Room: their plugin folders
		// go (game files on the hard drive, own games and saves stay).
		var oldNote string
		if old, err := presentOldGamePlugins(target); err != nil {
			checked.Result = remountNote + "Could not check for the old separate game entries: " + err.Error()
			checked.Ready = false
			return checked
		} else if len(old) > 0 {
			if _, removed, err := uninstall(target, old); err != nil {
				checked.Result = remountNote + "Could not replace the old separate game entries with Game Room: " + err.Error()
				checked.Ready = false
				return checked
			} else {
				oldNote = "Replaced the separate entries " + strings.Join(displayNames(removed), ", ") + " with Game Room (game files, your own games and saves kept).\n"
			}
		}
		remountNote += oldNote
		// Apps installed under the old folder names would show up in iDrive
		// next to the renamed ones, so they must be removed first.
		if legacy, err := presentLegacyApps(target); err != nil || len(legacy) > 0 {
			if err != nil {
				checked.Result = remountNote + "Could not check for old app versions: " + err.Error()
			} else {
				checked.Result = remountNote + "Old versions are still installed (" + strings.Join(legacy, ", ") +
					"). Select Uninstall everything first, restart iDrive, then install again."
			}
			checked.Ready = false
			return checked
		}
		selected, err := selectedApps(in.Apps)
		if err != nil {
			checked.Result = remountNote + err.Error()
			checked.Ready = false
			return checked
		}
		backup, err := install(target, in.Gateway, checked.screen, recoverLock, selected)
		if err != nil {
			checked.Result = "Install stopped. Leave iDrive running for recovery. " + err.Error()
			var blocked *blockedInstall
			if errors.As(err, &blocked) {
				checked.CanRecover = blocked.canRecover
				checked.CanUnlockEmpty = blocked.canUnlockEmpty
			}
			if backup != "" {
				checked.Result += " Staging or backups are at " + backup + "."
			}
		} else {
			names := make([]string, len(selected))
			for i, app := range selected {
				names[i] = displayName(app.name)
			}
			checked.Result = "Installed and verified " + strings.Join(names, ", ") + ". " + restartNote(" After iDrive starts again, open My Vehicle.")
			if len(gameNotes) > 0 {
				checked.Result += "\n" + strings.Join(gameNotes, "\n")
			}
		}
		checked.Ready = false
	}
	checked.Result = remountNote + checked.Result
	return checked
}

// connectToCar wraps sshConnect, turning an unconfirmed or changed host key
// into the same two-step trust prompt inspect/install already use (show the
// fingerprint, wait for the user to confirm, then connect for real). ok is
// false whenever there's nothing to connect to yet and trust is what the
// caller should show instead - this lets any action, including a first-ever
// Pull diagnostics, establish the SSH connection itself rather than requiring
// Check compatibility to have already run.
func connectToCar(in vehicleInput) (target remote, trust inspection, ok bool) {
	client, socket, fingerprint, err := sshConnect(in)
	if err != nil {
		trust = inspection{Result: err.Error()}
		if fingerprint != "" && fingerprint != in.Fingerprint {
			trust.Fingerprint = fingerprint
			trust.Result = "Verify the SSH fingerprint for this head unit before continuing."
			if in.Fingerprint != "" {
				trust.Fingerprint = ""
				trust.Result = "The SSH key changed. Confirm that the IP still belongs to your head unit, then start again."
			}
		}
		return remote{}, trust, false
	}
	return remote{client, socket}, inspection{}, true
}

// restartNote tells the user to restart iDrive by hand; after is appended.
// (An automatic restart was tried and removed: this unit has no usable
// shutdown command reachable over SSH.)
func restartNote(after string) string {
	return "While parked, hold the volume/power knob until the iDrive screen goes dark (about 30 seconds), then release." + after
}
