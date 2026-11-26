package main

import (
	"encoding/base64"
	"encoding/json"
	"os"
	"path/filepath"
)

// savedConnection is what the "remember" tick box persists between runs.
// The password is never written in plain text - only its DPAPI-protected
// form, which only decrypts under the same Windows user account.
type savedConnection struct {
	Head, Gateway, Version string
	PasswordProtected      string // base64 of CryptProtectData output
	// Display is the resolution choice: "" (auto), "800x480", "1280x480",
	// "1440x540", "1920x720" or "custom" with Width/Height.
	Display       string `json:",omitempty"`
	Width, Height int    `json:",omitempty"`
	// Remembered is true only when the "remember" box was ticked for this
	// save - distinct from "a settings file exists", since Theme/Language
	// below keep the file around even with the box unticked.
	Remembered bool `json:",omitempty"`
	// Theme ("light"/"dark") and Language (a code from webLanguages) are the
	// View menu's choices. Unlike the rest of this file, they are saved
	// unconditionally - they are preferences, not connection secrets, so the
	// "remember" tick box (which only governs Head/Gateway/Version/Password)
	// does not gate them.
	Theme    string `json:",omitempty"`
	Language string `json:",omitempty"`
}

func settingsPath() (string, error) {
	dir, err := os.UserConfigDir()
	if err != nil {
		return "", err
	}
	dir = filepath.Join(dir, "ID6AppInstaller")
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return "", err
	}
	return filepath.Join(dir, "connection.json"), nil
}

// saveConnection overwrites the saved connection details. Called whenever
// an action runs with the "remember" box ticked, so it always reflects
// whatever was last actually used.
func saveConnection(saved savedConnection, password string) error {
	path, err := settingsPath()
	if err != nil {
		return err
	}
	saved.Remembered = true
	// Theme/Language are not part of what any action saves - keep whatever
	// is already on disk rather than blanking them.
	if existing, ok := loadSavedConnection(); ok {
		saved.Theme = existing.Theme
		saved.Language = existing.Language
	}
	saved.PasswordProtected = ""
	if password != "" {
		protected, err := dpapiProtect([]byte(password))
		if err == nil {
			saved.PasswordProtected = base64.StdEncoding.EncodeToString(protected)
		}
	}
	data, err := json.Marshal(saved)
	if err != nil {
		return err
	}
	return os.WriteFile(path, data, 0o600)
}

// loadSavedConnection returns the saved details, if any. A password that
// fails to decrypt (e.g. the file was copied to a different machine or
// user account) is silently dropped rather than treated as an error - the
// IPs and version are still useful on their own.
func loadSavedConnection() (savedConnection, bool) {
	path, err := settingsPath()
	if err != nil {
		return savedConnection{}, false
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return savedConnection{}, false
	}
	var saved savedConnection
	if err := json.Unmarshal(data, &saved); err != nil {
		return savedConnection{}, false
	}
	return saved, true
}

func decryptSavedPassword(saved savedConnection) string {
	if saved.PasswordProtected == "" {
		return ""
	}
	cipher, err := base64.StdEncoding.DecodeString(saved.PasswordProtected)
	if err != nil {
		return ""
	}
	plain, err := dpapiUnprotect(cipher)
	if err != nil {
		return ""
	}
	return string(plain)
}

// clearSavedConnection erases the saved connection secret - called whenever
// the "remember" box is unticked, so unchecking it actually clears the
// secret, not just stops updating it. Theme/Language are kept: they are not
// the secret this box controls.
func clearSavedConnection() {
	path, err := settingsPath()
	if err != nil {
		return
	}
	existing, _ := loadSavedConnection()
	cleared := savedConnection{Theme: existing.Theme, Language: existing.Language}
	data, err := json.Marshal(cleared)
	if err != nil {
		return
	}
	_ = os.WriteFile(path, data, 0o600)
}

// savePreferences updates just the theme/language, preserving whatever
// connection details are (or are not) already saved.
func savePreferences(theme, language string) error {
	path, err := settingsPath()
	if err != nil {
		return err
	}
	saved, _ := loadSavedConnection()
	saved.Theme = theme
	saved.Language = language
	data, err := json.Marshal(saved)
	if err != nil {
		return err
	}
	return os.WriteFile(path, data, 0o600)
}
