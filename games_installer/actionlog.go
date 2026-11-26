package main

// A local log of every installer action, so a failure can be traced afterwards (owner's request,
// 2026-11-18, after installs with every app ticked failed with an SSH handshake timeout and nothing
// recorded what had happened). It lives beside the saved settings
// (%AppData%\ID6AppInstaller\actions.log, one .previous kept) and a copy goes into every diagnostics
// pull folder as installer-actions.log.
//
// What it records: each action's start (action, ticked apps, head unit / gateway IPs, display), every
// SSH connect attempt with its timing and error, every progress line, and every result sent to the
// window. Never the password: actionInput's Password is not logged, and no result carries it.

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

const (
	actionLogName  = "actions.log"
	actionLogLimit = 512 << 10 // then rotated to actions.log.previous
	actionLogLine  = 1500      // longest single entry; diagnostics reports are cut here
)

var actionLogMu sync.Mutex

func actionLogPath() (string, error) {
	settings, err := settingsPath()
	if err != nil {
		return "", err
	}
	return filepath.Join(filepath.Dir(settings), actionLogName), nil
}

// logAction appends one timestamped line. Best effort: a log that cannot be written never stops
// an action.
func logAction(format string, args ...interface{}) {
	text := fmt.Sprintf(format, args...)
	text = strings.ReplaceAll(strings.ReplaceAll(text, "\r", ""), "\n", " | ")
	if len(text) > actionLogLine {
		text = text[:actionLogLine] + fmt.Sprintf(" ...(%d more bytes)", len(text)-actionLogLine)
	}
	path, err := actionLogPath()
	if err != nil {
		return
	}
	actionLogMu.Lock()
	defer actionLogMu.Unlock()
	if info, err := os.Stat(path); err == nil && info.Size() > actionLogLimit {
		_ = os.Rename(path, path+".previous")
	}
	f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o600)
	if err != nil {
		return
	}
	defer f.Close()
	fmt.Fprintf(f, "%s %s\n", time.Now().Format("2006-01-02 15:04:05.000"), text)
}

// logActionStart records what an action was asked to do - everything in actionInput except the
// password, which is never written anywhere in plain text.
func logActionStart(action string, in actionInput) {
	logAction("START %s head=%s gateway=%s local=%s version=%s apps=%v uninstall=%v display=%s remount=%v trusted_key=%v",
		action, in.Head, in.Gateway, in.Local, in.Version, in.Apps, in.UninstallApps, in.Display, in.Remount,
		in.Fingerprint != "")
}

// logActionEvent records one event sent to the window (a progress line or a result).
func logActionEvent(kind string, payload interface{}) {
	data, err := json.Marshal(payload)
	if err != nil {
		data = []byte(fmt.Sprintf("%v", payload))
	}
	logAction("EVENT %s %s", kind, data)
}

// copyActionLog puts the log (and its .previous) into a diagnostics pull folder. Returns a line
// for the pull summary.
func copyActionLog(dir string) string {
	path, err := actionLogPath()
	if err != nil {
		return "Installer action log: unavailable (" + err.Error() + ")."
	}
	actionLogMu.Lock()
	defer actionLogMu.Unlock()
	var parts []byte
	if old, err := os.ReadFile(path + ".previous"); err == nil {
		parts = append(parts, old...)
	}
	current, err := os.ReadFile(path)
	if err == nil {
		parts = append(parts, current...)
	}
	if len(parts) == 0 {
		return "Installer action log: nothing recorded yet."
	}
	if err := os.WriteFile(filepath.Join(dir, "installer-actions.log"), parts, 0o644); err != nil {
		return "Installer action log: could not be copied (" + err.Error() + ")."
	}
	return "Saved the installer's action log as installer-actions.log."
}
