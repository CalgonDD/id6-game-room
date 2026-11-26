package main

// flash.go: keeping an eye on the car's flash (owner, 2026-11-20). The plugins and every game's saves live on the
// /var partition (eMMC, ~492 MiB, shared with the iDrive itself). Retro refuses a save state that would leave less
// than 100 MB free (src/nes_worker.c, NS_FLASH_RESERVE); here the installer shows how much is free and what the
// saves take, warns well before that, and can delete save states (the big ones: a PS1 one is ~4-5 MB). Memory
// cards, battery saves and the other games' saves are never touched.

import (
	"fmt"
	"strconv"
	"strings"
)

const flashWarnMB = 150 // warn below this (Retro itself refuses save states below 100 MB)

// gameSaveDirs are the games' save folders on the flash.
var gameSaveDirs = []string{"Freedoom-data", "Tyrian-data", "LibreQuake-data", "Retro-data"}

type flashInfo struct {
	ok, gotSave       bool
	totalMB, freeMB   float64
	savesMB, statesMB float64
	states            int
}

// parseDf reads QNX df output for one path: device, total, used, free (512-byte blocks), percent, mount.
func parseDf(out string) (totalMB, freeMB float64, ok bool) {
	for _, line := range strings.Split(out, "\n") {
		f := strings.Fields(line)
		if len(f) < 4 {
			continue
		}
		total, e1 := strconv.ParseFloat(f[1], 64)
		free, e2 := strconv.ParseFloat(f[3], 64)
		if e1 == nil && e2 == nil && total > 0 {
			return total * 512 / (1 << 20), free * 512 / (1 << 20), true
		}
	}
	return 0, 0, false
}

// parseDu adds up "blocks<TAB>path" lines (512-byte blocks) and counts them.
func parseDu(out string) (mb float64, n int) {
	for _, line := range strings.Split(out, "\n") {
		f := strings.Fields(line)
		if len(f) < 2 {
			continue
		}
		if b, err := strconv.ParseFloat(f[0], 64); err == nil {
			mb += b * 512 / (1 << 20)
			n++
		}
	}
	return mb, n
}

// flashStatus asks the car (read only).
func flashStatus(r remoteRunner) flashInfo {
	var info flashInfo
	out, err := r.run("df "+root+" 2>/dev/null", nil, 4096)
	if err == nil {
		info.totalMB, info.freeMB, info.ok = parseDf(string(out))
	}
	if !info.ok {
		// Some firmware df builds reject a subdirectory. The supported head unit
		// exposes /var on /fs/sda1; never use the first df line (it can be RAM/HDD).
		if all, readErr := r.run("df 2>/dev/null", nil, 16384); readErr == nil {
			for _, line := range strings.Split(string(all), "\n") {
				fields := strings.Fields(line)
				if len(fields) < 6 {
					continue
				}
				mount := strings.TrimRight(fields[len(fields)-1], "/")
				if mount != "/fs/sda1" && mount != "/var" {
					continue
				}
				info.totalMB, info.freeMB, info.ok = parseDf(line)
				if info.ok {
					break
				}
			}
		}
	}
	if !info.ok {
		return info
	}
	var script strings.Builder
	fmt.Fprintf(&script, "root=%s\n", shellQuote(root))
	for _, d := range gameSaveDirs {
		fmt.Fprintf(&script, "test -d \"$root/%s\" && du -s \"$root/%s\" 2>/dev/null\n", d, d)
	}
	script.WriteString("echo states\nfor f in \"$root\"/Retro-data/*.state; do test -f \"$f\" && du -s \"$f\" 2>/dev/null; done\nexit 0\n")
	if out, err = r.run("/bin/sh -s", []byte(script.String()), 1<<16); err == nil {
		parts := strings.SplitN(string(out), "states\n", 2)
		info.savesMB, _ = parseDu(parts[0])
		if len(parts) == 2 {
			info.statesMB, info.states = parseDu(parts[1])
		}
		info.gotSave = true
	}
	return info
}

// flashLine is the report shown with Check, Show games and the save-state actions.
func flashLine(info flashInfo) string {
	if !info.ok {
		return "Flash: free-space reading unavailable. This does not mean the flash is full."
	}
	line := fmt.Sprintf("Flash: %.0f MB free of %.0f MB.", info.freeMB, info.totalMB)
	if info.gotSave {
		line += fmt.Sprintf(" Game saves: %.1f MB (save states %.1f MB in %d file(s)).", info.savesMB, info.statesMB, info.states)
	}
	if info.freeMB < flashWarnMB {
		line += fmt.Sprintf("\nWARNING: the flash is getting full (under %d MB free). Game Room stops making save states below 100 MB; Delete save states in Your games frees that space (memory cards and in-game saves are kept).", flashWarnMB)
	}
	return line
}

// deleteSaveStates removes Retro's save states (*.state) only; memory cards (.srm) and every other save stay.
func deleteSaveStates(r remoteRunner) (int, error) {
	script := "root=" + shellQuote(root) + `
n=0
for f in "$root"/Retro-data/*.state "$root"/Retro-data/*.state.new; do
    test -f "$f" && test ! -L "$f" || continue
    rm -f "$f" && n=$((n+1))
done
echo "removed=$n"
`
	out, err := runMutation(r, "/bin/sh -s", []byte(script), 1024)
	if err != nil {
		return 0, err
	}
	n := 0
	fmt.Sscanf(strings.TrimSpace(string(out)), "removed=%d", &n)
	return n, nil
}
