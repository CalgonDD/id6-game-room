package main

import (
	"embed"
	"fmt"
	"io/fs"
	"net"
	"net/http"
	"os"

	webview2 "github.com/jchv/go-webview2"
)

//go:embed web
var webAssets embed.FS

// buildLabel names the build in the window title (set at link time by tools/build_release.py
// from ID6_BUILD_LABEL, e.g. "Game Room v33.2").
var buildLabel = ""

// The page is served from a real loopback HTTP server, not SetHtml/a data
// URI: WebView2 treats SetHtml content as the opaque "about:blank" origin,
// which blocks the per-origin storage i18n/theme prefs would otherwise sit
// in, and a data URI has the same problem plus a practical size limit. A
// plain http.Handler over embed.FS needs neither; the listener is loopback
// and closes with the process, so nothing else on the PC can reach it.
func startAssetServer() (string, error) {
	sub, err := fs.Sub(webAssets, "web")
	if err != nil {
		return "", err
	}
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return "", err
	}
	go http.Serve(listener, http.FileServer(http.FS(sub)))
	return "http://" + listener.Addr().String(), nil
}

func windowTitle() string {
	if buildLabel == "" {
		return "ID6 Game Room Installer"
	}
	return "ID6 Game Room Installer - " + buildLabel
}

func main() {
	url, err := startAssetServer()
	if err != nil {
		fmt.Fprintln(os.Stderr, "could not start the UI asset server:", err)
		os.Exit(1)
	}

	w := webview2.NewWithOptions(webview2.WebViewOptions{
		AutoFocus: true,
		WindowOptions: webview2.WindowOptions{
			Title:  windowTitle(),
			Width:  1360,
			Height: 860,
			Center: true,
		},
	})
	if w == nil {
		fmt.Fprintln(os.Stderr, "could not create the application window - is the WebView2 runtime installed?")
		os.Exit(1)
	}
	defer w.Destroy()
	// A *minimum* size, not the fixed size above: shrinking the window below
	// this is still possible and falls back to the page's own responsive
	// layout (single column, internal scrolling) rather than being blocked
	// outright.
	w.SetSize(480, 600, webview2.HintMin)

	a := &app{w: w}
	bind := func(name string, f interface{}) {
		if err := w.Bind(name, f); err != nil {
			fmt.Fprintln(os.Stderr, "bind", name, err)
		}
	}
	bind("goBootstrap", a.bootstrap)
	bind("goSetTheme", a.setTheme)
	bind("goSetLanguage", a.setLanguage)
	bind("goClearRemembered", a.clearRemembered)
	bind("goFindCar", a.findCar)
	bind("goConnect", a.connect)
	bind("goDoAction", a.doAction)
	bind("goPullDiagnostics", a.pullDiagnostics)
	bind("goChooseGames", a.chooseGames)
	bind("goQuit", a.quit)

	w.Navigate(url + "/index.html")
	w.Run()
}
