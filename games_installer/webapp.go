package main

// The WebView2 front-end's Go side: a thin, mostly stateless bridge between
// the HTML/CSS/JS UI (web/) and the existing vehicle logic in install.go,
// enet.go and vehicle_actions.go, none of which changed for this redesign.
//
// Unlike the old lxn/walk UI, this bridge keeps almost no state of its own -
// the fingerprint-trust flow, "busy" button state, and the detected-car list
// all live in the page's own JavaScript now, since every bound call already
// receives everything it needs and returns a complete result. The one
// exception is the webview handle itself, needed to push progress/result
// events back into the page from a background goroutine.
//
// Every "do a thing on the car" bound method returns a string immediately:
// "" means "started - watch for the matching window.__ipcEvent", anything
// else is an error code the page already has a translation for (see
// web/app.js's errorCodes). The real result always arrives later through
// that event, emitted via webview.Dispatch+Eval from the goroutine actually
// doing the work, so the UI thread is never blocked by a slow SSH call.

import (
	"encoding/json"
	"fmt"
	"strings"
	"sync"

	webview2 "github.com/jchv/go-webview2"
)

type app struct {
	w webview2.WebView
	// carBusy single-flights every action that opens an SSH session to the
	// head unit (connect/doAction/pullDiagnostics). The JS side already sets
	// a "busy" flag to stop a second click, but that only guards the one
	// button that was clicked - a second tab, a stale retry, or a race
	// between the JS flag and an in-flight request could still open a
	// second SSH session, and this car's temporary debug SSH only tolerates
	// one at a time (see the "handshake failed" / dropped-session reports
	// from testing with more than one session open). findCar() isn't
	// guarded: discover() is a UDP broadcast, not an SSH session.
	carBusy sync.Mutex
}

// withCar runs action only if no other car-touching action is already in
// flight; otherwise it reports ok=false immediately so the caller can emit a
// clear "busy" result instead of blocking or silently queueing behind
// whatever is already using the car's one SSH session.
func (a *app) withCar(action func()) (ok bool) {
	if !a.carBusy.TryLock() {
		return false
	}
	go func() {
		defer a.carBusy.Unlock()
		action()
	}()
	return true
}

// emit pushes one event into the page: window.__ipcEvent(kind, payload).
// Safe to call from any goroutine - it hops onto the UI thread itself.
func (a *app) emit(kind string, payload interface{}) {
	logActionEvent(kind, payload)
	kindJSON, err := json.Marshal(kind)
	if err != nil {
		return
	}
	payloadJSON, err := json.Marshal(payload)
	if err != nil {
		payloadJSON, _ = json.Marshal(fmt.Sprintf("%v", payload))
	}
	a.w.Dispatch(func() {
		a.w.Eval(fmt.Sprintf("window.__ipcEvent(%s, %s)", string(kindJSON), string(payloadJSON)))
	})
}

// ---- bootstrap: everything the page needs to render its first frame ----

type diagSectionInfo struct {
	Key, Label string
}

type appInfo struct {
	Name, Display string
}

type bootstrapData struct {
	Theme, Language        string
	Remembered             bool
	Head, Gateway, Version string
	Password               string
	Display                string
	Width, Height          int
	DiagSections           []diagSectionInfo
	Apps                   []appInfo
}

func (a *app) bootstrap() (bootstrapData, error) {
	data := bootstrapData{Theme: "dark"}
	if saved, ok := loadSavedConnection(); ok {
		if saved.Theme != "" {
			data.Theme = saved.Theme
		}
		data.Language = saved.Language
		if saved.Remembered {
			data.Remembered = true
			data.Head = saved.Head
			data.Gateway = saved.Gateway
			data.Version = saved.Version
			data.Password = decryptSavedPassword(saved)
			data.Display = saved.Display
			data.Width = saved.Width
			data.Height = saved.Height
		}
	}
	for _, s := range diagSections {
		data.DiagSections = append(data.DiagSections, diagSectionInfo{Key: s.key, Label: s.label})
	}
	for _, p := range apps {
		data.Apps = append(data.Apps, appInfo{Name: p.name, Display: displayName(p.name)})
	}
	return data, nil
}

func (a *app) setTheme(theme string) (bool, error) {
	if theme != "light" && theme != "dark" {
		return false, fmt.Errorf("unknown theme %q", theme)
	}
	saved, _ := loadSavedConnection()
	return true, savePreferences(theme, saved.Language)
}

func (a *app) setLanguage(lang string) (bool, error) {
	saved, _ := loadSavedConnection()
	return true, savePreferences(saved.Theme, lang)
}

func (a *app) clearRemembered() (bool, error) {
	clearSavedConnection()
	return true, nil
}

func (a *app) quit() (bool, error) {
	// Dispatch: Terminate stops Run()'s loop, but doing it from inside this
	// same synchronous bound call would tear down the window before the
	// call's own JSON response could be delivered back to the page.
	a.w.Dispatch(func() { a.w.Terminate() })
	return true, nil
}

// ---- shared input shape for every action that talks to the car ----

type actionInput struct {
	Head, Gateway, Local, Version, Password, Fingerprint string
	Apps, UninstallApps                                  []string
	OwnGames, RemoveGames                                []string
	Display                                              string
	Width, Height                                        int
	Remember                                             bool
	Remount                                              bool
}

// buildVehicleInput mirrors the old ui.buildInput(): validates, fills in the
// custom-resolution string, and saves the connection when asked to. Returns
// an error *code* (not an English sentence) so the page can localize it.
func buildVehicleInput(in actionInput) (vehicleInput, string) {
	if in.Head == "" || in.Gateway == "" {
		return vehicleInput{}, "err_need_ips"
	}
	out := vehicleInput{
		Head:          in.Head,
		Gateway:       in.Gateway,
		Local:         in.Local,
		Version:       in.Version,
		Fingerprint:   in.Fingerprint,
		Password:      in.Password,
		Apps:          in.Apps,
		UninstallApps: in.UninstallApps,
		OwnGames:      in.OwnGames,
		RemoveGames:   in.RemoveGames,
	}
	out.DisplayResolution = in.Display
	if in.Display == "custom" {
		if in.Width < 800 || in.Width > 1920 || in.Height < 480 || in.Height > 720 {
			return vehicleInput{}, "err_need_custom_size"
		}
		out.DisplayResolution = fmt.Sprintf("%dx%d", in.Width, in.Height)
	}
	if in.Remember {
		saved := savedConnection{Head: out.Head, Gateway: out.Gateway, Version: out.Version, Display: in.Display}
		if in.Display == "custom" {
			saved.Width, saved.Height = in.Width, in.Height
		}
		_ = saveConnection(saved, in.Password)
	}
	return out, ""
}

// ---- Find car ----

type findCarItem struct{ Gateway, Head, Local string }
type findCarResult struct {
	Error string
	Items []findCarItem
}

func (a *app) findCar() (string, error) {
	go func() {
		items, err := discover()
		result := findCarResult{}
		if err != nil {
			result.Error = err.Error()
		} else {
			for _, it := range items {
				result.Items = append(result.Items, findCarItem{Gateway: it.Gateway, Head: it.Head, Local: it.Local})
			}
		}
		a.emit("findcar", result)
	}()
	return "", nil
}

// ---- Connect (plain SSH reachability check, no inspection) ----

type connectResult struct {
	Ok          bool
	Result      string
	Fingerprint string
}

func (a *app) connect(in actionInput) (string, error) {
	logActionStart("connect", in)
	vin, errCode := buildVehicleInput(in)
	if errCode != "" {
		return errCode, nil
	}
	started := a.withCar(func() {
		target, trust, ok := connectToCar(vin)
		if !ok {
			a.emit("connect", connectResult{Ok: false, Result: trust.Result, Fingerprint: trust.Fingerprint})
			return
		}
		target.client.Close()
		a.emit("connect", connectResult{Ok: true, Result: "Connected. SSH is reachable and the host key is trusted."})
	})
	if !started {
		return "err_car_busy", nil
	}
	return "", nil
}

// ---- The generic action dispatcher: inspect/install/recover/uninstall/
// uninstallselected/deletebackups all return an `inspection`, same as the
// old UI's applyResult expected. ----

func (a *app) doAction(action string, in actionInput) (string, error) {
	logActionStart(action, in)
	vin, errCode := buildVehicleInput(in)
	if errCode != "" {
		return errCode, nil
	}
	started := a.withCar(func() {
		result := doVehicleAction(action, vin, in.Remount)
		a.emit(action, result)
	})
	if !started {
		return "err_car_busy", nil
	}
	return "", nil
}

// ---- Diagnostics ----

type diagnosticsResult struct {
	Error       string
	Text        string
	Fingerprint string
}

func (a *app) pullDiagnostics(in actionInput, sections map[string]bool, crashDump, libraries bool) (string, error) {
	logActionStart(fmt.Sprintf("diagnostics sections=%v crash_dump=%v libraries=%v", sections, crashDump, libraries), in)
	any := crashDump || libraries
	for _, on := range sections {
		any = any || on
	}
	if !any {
		return "err_need_diag_choice", nil
	}
	vin, errCode := buildVehicleInput(in)
	if errCode != "" {
		return errCode, nil
	}
	started := a.withCar(func() {
		target, trust, ok := connectToCar(vin)
		if !ok {
			a.emit("diagnostics", diagnosticsResult{Error: trust.Result, Fingerprint: trust.Fingerprint})
			return
		}
		progress := func(text string) { a.emit("progress", map[string]string{"Text": text}) }
		// One timed-out step kills the SSH transport; the link redials (same host-key check)
		// before the next step instead of letting every later step fail.
		link := &carLink{current: target, progress: progress, dial: func() (linkConn, bool) {
			next, _, ok := connectToCar(vin)
			return next, ok
		}}
		defer link.close()
		summary, report, err := gatherDiagnostics(link, diagChoice{sections: sections, crashDump: crashDump, libraries: libraries}, progress)
		if err != nil {
			a.emit("diagnostics", diagnosticsResult{Error: err.Error()})
			return
		}
		a.emit("diagnostics", diagnosticsResult{Text: windowReport(summary, report)})
	})
	if !started {
		return "err_car_busy", nil
	}
	return "", nil
}

// diagnosticsWindowLimit caps how much of a pull's report goes to the window. A full pull is 500-700 KB, and
// that whole string went through one webview Eval; the owner saw the window stay on "Collecting the selected
// diagnostics..." twice although the pull had finished and its folder was complete (2026-11-18). The full
// report is always in report.txt in the saved folder, which the summary names.
const diagnosticsWindowLimit = 60000

// windowReport is the summary plus as much of the report as fits, cut at a line break.
func windowReport(summary, report string) string {
	if len(report) <= diagnosticsWindowLimit {
		return summary + "\n\n" + report
	}
	cut := strings.LastIndex(report[:diagnosticsWindowLimit], "\n")
	if cut < 0 {
		cut = diagnosticsWindowLimit
	}
	return fmt.Sprintf("%s\n\n%s\n\n... (%d more characters not shown here: the full report is report.txt in the folder above)",
		summary, report[:cut], len(report)-cut)
}

// ---- Your games: choose and check files on this PC (no car connection needed) ----

type ownCheckResult struct {
	Canceled bool
	Skipped  int
	Games    []ownGame
}

// chooseGames opens the Windows file or folder picker (modal, on the window's own thread: bound calls run
// there) and checks what was chosen.
func (a *app) chooseGames(kind string) (ownCheckResult, error) {
	var paths []string
	if kind == "folder" {
		if p := pickFolder(a.w.Window(), "Choose a folder with your games"); p != "" {
			paths = []string{p}
		}
	} else {
		paths = pickFiles(a.w.Window(), "Choose your game files")
	}
	if len(paths) == 0 {
		return ownCheckResult{Canceled: true}, nil
	}
	games, skipped := checkOwnGames(paths)
	return ownCheckResult{Skipped: skipped, Games: games}, nil
}
