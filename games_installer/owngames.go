package main

// owngames.go: "Your games" (2026-11-20). The owner picks game files on this PC (files, a folder, or .zip
// archives); each one is checked by what is inside it, not by its name, and the ones a game here can play are
// copied to that game's yours/ folder on the car's hard drive, where its game list shows them (marked *):
//
//	Retro       NES (.nes, iNES header), Game Boy / Color (.gb/.gbc, cartridge header logo and checksum)
//	Freedoom    Doom-engine IWADs (Doom, Doom II, Final Doom, Chex Quest, HacX)
//	LibreQuake  Quake's id1 pak0/pak1 and the two mission packs (hipnotic, rogue)
//
// The copy goes through the same staging and one-writable-window publish as the bundled game files
// (putGameFiles in games.go). Uninstalling a game keeps its yours/ folder; "Remove ticked games" deletes them.

import (
	"archive/zip"
	"bytes"
	"crypto/md5"
	cryptorand "crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
)

// ownGame is one file the owner chose, after checking.
type ownGame struct {
	Key     string // unique within one check
	File    string // what the owner chose (the file, or "archive.zip > member")
	Game    string // its folder under ID6Games: "Retro", "Freedoom", "LibreQuake"; "" when it can't be used
	App     string // the My Vehicle entry it shows up in: "Retro" or "Game Room"
	Sub     string // folder under the game's folder on the hard drive: "yours", "yours/id1", ...
	Name    string // file name on the car
	Title   string // what it is
	Size    int64
	Problem string // why it can't be added ("" = it can)
	Note    string
	source  string // the file on this PC (the archive, for a zip member)
	member  string // the file inside the archive, or ""
}

// ownGameDirs are the only folders own games go in (and the only ones Remove may touch).
var ownGameDirs = []string{"PSP/yours", "Retro/yours", "Freedoom/yours", "LibreQuake/yours/id1", "LibreQuake/yours/hipnotic", "LibreQuake/yours/rogue"}

const (
	ownMaxFile   = 512 << 20 // a file bigger than this is no game any of these engines plays
	ownMaxMember = 128 << 20 // zip members are read into memory
	ownMaxFiles  = 3000      // files looked at in one folder
	ownMaxDisc   = 2 << 30   // a PS1 disc image (.chd) can be up to this
)

var ownChecked struct {
	sync.Mutex
	list []ownGame
}

// gameExtensions are the files a folder scan reports even when they turn out unusable (so the owner learns
// why); anything else in a folder that isn't a game is skipped quietly.
var gameExtensions = map[string]bool{
	".nes": true, ".gb": true, ".gbc": true, ".gba": true, ".wad": true, ".pak": true, ".zip": true, ".7z": true,
	".rar": true, ".sfc": true, ".smc": true, ".md": false, ".gen": true, ".smd": true, ".sms": true, ".gg": true,
	".n64": true, ".z64": true, ".v64": true, ".nds": true, ".cso": true, ".iso": true, ".cue": true, ".chd": true, ".pbp": true,
}

// checkOwnGames checks chosen files and folders and remembers the result for addOwnGames.
func checkOwnGames(paths []string) (games []ownGame, skipped int) {
	for _, p := range paths {
		info, err := os.Stat(p)
		if err != nil {
			games = append(games, ownGame{File: p, Problem: "Could not open it: " + err.Error()})
			continue
		}
		if !info.IsDir() {
			games = append(games, checkOwnFile(p, info.Size())...)
			continue
		}
		seen := 0
		_ = filepath.WalkDir(p, func(path string, d fs.DirEntry, err error) error {
			if err != nil || d.IsDir() {
				return nil
			}
			if seen++; seen > ownMaxFiles {
				return filepath.SkipAll
			}
			ext := strings.ToLower(filepath.Ext(path))
			fi, err := d.Info()
			if err != nil {
				return nil
			}
			found := checkOwnFile(path, fi.Size())
			usable := false
			for _, g := range found {
				usable = usable || g.Problem == ""
			}
			if usable || gameExtensions[ext] {
				games = append(games, found...)
			} else {
				skipped++
			}
			return nil
		})
	}
	games = settleOwnGames(games)
	ownChecked.Lock()
	ownChecked.list = games
	ownChecked.Unlock()
	return games, skipped
}

// checkOwnFile checks one file on this PC; a .zip gives one entry per file inside it.
func checkOwnFile(path string, size int64) []ownGame {
	base := filepath.Base(path)
	g := ownGame{File: path, Size: size, source: path}
	if size > ownMaxFile && !((strings.EqualFold(filepath.Ext(base), ".chd") || strings.EqualFold(filepath.Ext(base), ".iso") || strings.EqualFold(filepath.Ext(base), ".cso") || strings.EqualFold(filepath.Ext(base), ".pbp")) && size <= ownMaxDisc) {
		g.Problem = "Too big for a game these emulators and engines play."
		return []ownGame{g}
	}
	f, err := os.Open(path)
	if err != nil {
		g.Problem = "Could not open it: " + err.Error()
		return []ownGame{g}
	}
	defer f.Close()
	head := make([]byte, 4)
	_, _ = io.ReadFull(f, head)
	if string(head) == "PK\x03\x04" {
		return checkOwnZip(path, f, size)
	}
	classifyOwnGame(&g, base, f, size)
	return []ownGame{g}
}

func checkOwnZip(path string, f io.ReaderAt, size int64) []ownGame {
	z, err := zip.NewReader(f, size)
	if err != nil {
		return []ownGame{{File: path, Size: size, Problem: "A .zip that could not be read: " + err.Error()}}
	}
	var out []ownGame
	for _, m := range z.File {
		if m.FileInfo().IsDir() {
			continue
		}
		g := ownGame{File: filepath.Base(path) + " > " + m.Name, Size: int64(m.UncompressedSize64), source: path, member: m.Name}
		if m.UncompressedSize64 > ownMaxMember {
			g.Problem = "Too big to read from inside a .zip; unpack it and choose the file itself."
			out = append(out, g)
			continue
		}
		data, err := readZipMember(m)
		if err != nil {
			g.Problem = "Could not read it from the .zip: " + err.Error()
			out = append(out, g)
			continue
		}
		classifyOwnGame(&g, filepath.Base(m.Name), bytes.NewReader(data), int64(len(data)))
		if g.Problem == "" || gameExtensions[strings.ToLower(filepath.Ext(m.Name))] {
			out = append(out, g)
		}
	}
	if len(out) == 0 {
		return []ownGame{{File: path, Size: size, Problem: "No game files inside this .zip."}}
	}
	return out
}

func readZipMember(m *zip.File) ([]byte, error) {
	rc, err := m.Open()
	if err != nil {
		return nil, err
	}
	defer rc.Close()
	return io.ReadAll(io.LimitReader(rc, ownMaxMember+1))
}

// load reads the file's bytes (from the archive for a zip member).
func (g ownGame) load() ([]byte, error) {
	if g.member == "" {
		return os.ReadFile(g.source)
	}
	z, err := zip.OpenReader(g.source)
	if err != nil {
		return nil, err
	}
	defer z.Close()
	for _, m := range z.File {
		if m.Name == g.member {
			return readZipMember(m)
		}
	}
	return nil, fmt.Errorf("%s is no longer in %s", g.member, filepath.Base(g.source))
}

// gbLogo is the logo every Game Boy cartridge carries at 0x104 (the boot ROM refuses a cartridge without it).
var gbLogo = []byte{0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
	0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
	0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E}

var doomTitles = map[string]string{
	"doom.wad": "Doom", "doom1.wad": "Doom (shareware)", "doom2.wad": "Doom II", "tnt.wad": "Final Doom: TNT Evilution",
	"plutonia.wad": "Final Doom: Plutonia", "chex.wad": "Chex Quest", "hacx.wad": "HacX",
}

func readAt(r io.ReaderAt, off int64, n int, size int64) []byte {
	if off < 0 || n < 0 || off+int64(n) > size {
		return nil
	}
	b := make([]byte, n)
	if _, err := r.ReadAt(b, off); err != nil && !errors.Is(err, io.EOF) {
		return nil
	}
	return b
}

// classifyOwnGame decides, from the contents, which game plays the file and where it goes.
func classifyOwnGame(g *ownGame, base string, r io.ReaderAt, size int64) {
	ext := strings.ToLower(filepath.Ext(base))
	stem := strings.TrimSuffix(base, filepath.Ext(base))
	head := readAt(r, 0, 16, size)
	if isPSPCHD(r, size) {
		classifyPSP(g, stem, ".chd", r, size)
		return
	}
	if ext == ".iso" || ext == ".cso" || ext == ".pbp" {
		classifyPSP(g, stem, ext, r, size)
		return
	}
	if size == 512*1024 {
		if name, ok := ps1Bios(r); ok {
			g.Game, g.Sub, g.Name, g.Title = "Retro", "yours", name+".bin", "PlayStation BIOS ("+strings.ToUpper(name)+")"
			g.Note = "Lets PlayStation games start the way a real console does (better compatibility)."
			return
		}
	}
	switch {
	case head != nil && string(head[:8]) == "MComprHD" && size > 1024:
		g.Game, g.Sub, g.Name, g.Title = "Retro", "yours", carFileName(stem)+".chd", "PlayStation: "+carFileName(stem)
		g.Note = "Saves go on one shared PS1 memory card. A multi-disc game is one entry per disc: save at the disc change, then start the next disc."
		return
	case head != nil && string(head[:4]) == "NES\x1a" && size >= 16+16384:
		g.Game, g.Sub, g.Name, g.Title = "Retro", "yours", carFileName(stem)+".nes", "NES: "+carFileName(stem)
		return
	case head != nil && string(head[:4]) == "PACK":
		classifyQuakePak(g, base, r, size)
		return
	case head != nil && (string(head[:4]) == "IWAD" || string(head[:4]) == "PWAD"):
		classifyWad(g, base, r, size, string(head[:4]) == "IWAD")
		return
	}
	if hdr := readAt(r, 0x100, 0x50, size); hdr != nil && bytes.Equal(hdr[4:4+len(gbLogo)], gbLogo) {
		sum := byte(0)
		for _, b := range hdr[0x34:0x4D] {
			sum = sum - b - 1
		}
		if sum != hdr[0x4D] {
			g.Problem = "A Game Boy file whose header checksum is wrong (a damaged or patched file)."
			return
		}
		e := ".gb"
		kind := "Game Boy"
		if hdr[0x43]&0x80 != 0 {
			e, kind = ".gbc", "Game Boy Color"
		}
		g.Game, g.Sub, g.Name, g.Title = "Retro", "yours", carFileName(stem)+e, kind+": "+carFileName(stem)
		return
	}
	if b := readAt(r, 0xB2, 1, size); b != nil && b[0] == 0x96 && (ext == ".gba" || ext == ".agb") {
		g.Problem = "Game Boy Advance: Retro can't play these yet."
		return
	}
	if b := readAt(r, 0x100, 4, size); b != nil && string(b) == "SEGA" {
		g.Problem = "Sega Mega Drive / Genesis: Retro can't play these yet."
		return
	}
	switch ext {
	case ".sfc", ".smc":
		g.Problem = "Super Nintendo: Retro can't play these yet."
	case ".sms", ".gg":
		g.Problem = "Sega Master System / Game Gear: Retro can't play these yet."
	case ".n64", ".z64", ".v64", ".nds":
		g.Problem = "Too demanding for the car (Nintendo 64 / DS)."
	case ".iso", ".cue", ".bin", ".pbp", ".img":
		g.Problem = "A PlayStation disc as .bin/.cue/.iso: convert it to .chd (one file per disc, about half the size) and add that."
	case ".chd":
		g.Problem = "Named like a disc image (.chd), but it isn't one inside."
	case ".7z", ".rar":
		g.Problem = "Unpack it first: only .zip archives are read."
	case ".nes", ".gb", ".gbc":
		g.Problem = "Named like a " + strings.ToUpper(ext[1:]) + " game, but it isn't one inside (damaged, or a different format)."
	case ".wad", ".pak":
		g.Problem = "Named like a game data file, but it isn't one inside."
	default:
		g.Problem = "Not a game file Retro, Freedoom or LibreQuake can play."
	}
}

func classifyWad(g *ownGame, base string, r io.ReaderAt, size int64, iwad bool) {
	if !iwad {
		g.Problem = "Add-on levels (a PWAD): they need the game they were made for; not supported yet."
		return
	}
	hdr := readAt(r, 4, 8, size)
	n := int64(binary.LittleEndian.Uint32(hdr[0:4]))
	ofs := int64(binary.LittleEndian.Uint32(hdr[4:8]))
	dir := readAt(r, ofs, int(n*16), size)
	if n <= 0 || n > 100000 || dir == nil {
		g.Problem = "A damaged WAD file (its lump directory doesn't fit the file)."
		return
	}
	lumps := map[string]bool{}
	for i := int64(0); i < n; i++ {
		name := dir[i*16+8 : i*16+16]
		if k := bytes.IndexByte(name, 0); k >= 0 {
			name = name[:k]
		}
		lumps[strings.ToUpper(string(name))] = true
	}
	lower := strings.ToLower(base)
	switch {
	case lumps["FREEDOOM"]:
		g.Problem = "Freedoom: already included with the installer."
		return
	case lumps["ENDTEXT"] || lumps["BEHAVIOR"] || lumps["ENDSTRF"]:
		g.Problem = "Heretic, Hexen or Strife: the Doom engine here can't play these."
		return
	case !lumps["PLAYPAL"] || !(lumps["E1M1"] || lumps["MAP01"]):
		g.Problem = "Not a Doom-engine game the engine here knows."
		return
	}
	name := lower
	if _, known := doomTitles[lower]; !known {
		switch {
		case lumps["MAP01"] && strings.Contains(lower, "tnt"):
			name = "tnt.wad"
		case lumps["MAP01"] && strings.Contains(lower, "plut"):
			name = "plutonia.wad"
		case lumps["MAP01"]:
			name = "doom2.wad"
		case lumps["E2M1"]:
			name = "doom.wad"
		default:
			name = "doom1.wad"
		}
		g.Note = "Saved on the car as " + name + " (the engine knows the game by that name)."
	}
	g.Game, g.Sub, g.Name, g.Title = "Freedoom", "yours", name, doomTitles[name]
}

func classifyQuakePak(g *ownGame, base string, r io.ReaderAt, size int64) {
	hdr := readAt(r, 4, 8, size)
	ofs := int64(binary.LittleEndian.Uint32(hdr[0:4]))
	length := int64(binary.LittleEndian.Uint32(hdr[4:8]))
	dir := readAt(r, ofs, int(length), size)
	if length%64 != 0 || length > 64*200000 || dir == nil {
		g.Problem = "A damaged Quake pak (its directory doesn't fit the file)."
		return
	}
	files := map[string]bool{}
	for i := 0; i+64 <= len(dir); i += 64 {
		name := dir[i : i+56]
		if k := bytes.IndexByte(name, 0); k >= 0 {
			name = name[:k]
		}
		files[strings.ToLower(string(name))] = true
	}
	switch {
	case files["maps/hip1m1.bsp"]:
		g.Game, g.Sub, g.Name, g.Title = "LibreQuake", "yours/hipnotic", "pak0.pak", "Quake: Scourge of Armagon"
		g.Note = "Plays once Quake itself (its pak0.pak) is on the car too."
	case files["maps/r1m1.bsp"]:
		g.Game, g.Sub, g.Name, g.Title = "LibreQuake", "yours/rogue", "pak0.pak", "Quake: Dissolution of Eternity"
		g.Note = "Plays once Quake itself (its pak0.pak) is on the car too."
	case files["maps/start.bsp"] && files["maps/e1m1.bsp"] && files["progs.dat"]:
		g.Game, g.Sub, g.Name, g.Title = "LibreQuake", "yours/id1", "pak0.pak", "Quake"
		if !files["maps/e2m1.bsp"] {
			g.Note = "Episode 1 (shareware); add pak1.pak from the full game for episodes 2-4."
		}
	case files["maps/e2m1.bsp"] && !files["maps/start.bsp"]:
		g.Game, g.Sub, g.Name, g.Title = "LibreQuake", "yours/id1", "pak1.pak", "Quake: episodes 2-4"
		g.Note = "Plays once Quake's pak0.pak is on the car too."
	default:
		g.Problem = "A Quake pak that isn't Quake or one of its two mission packs (mods aren't supported yet)."
	}
}

// carFileName keeps a name readable but safe in the game list and on the car's file system.
func carFileName(stem string) string {
	var b strings.Builder
	for _, c := range strings.TrimSpace(stem) {
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9', strings.ContainsRune(" -_()[],.!&+'", c):
			b.WriteRune(c)
		default:
			b.WriteRune('_')
		}
	}
	s := strings.Trim(b.String(), " .")
	if len(s) > 60 {
		s = strings.TrimSpace(s[:60])
	}
	if s == "" {
		s = "game"
	}
	return s
}

// settleOwnGames gives every entry a key, drops files the installer already ships, and sorts out two files
// wanting the same place on the car.
func settleOwnGames(games []ownGame) []ownGame {
	taken := map[string]int{}
	for i := range games {
		g := &games[i]
		g.Key = fmt.Sprintf("g%d", i)
		if g.Problem != "" {
			continue
		}
		if bundledCopy(g) {
			g.Problem = "Already included with the installer."
			continue
		}
		// Retro's list shows names without the extension, so "X.gb" and "X.gbc" would look the same there
		key := func(name string) string {
			if g.Game == "Retro" {
				name = strings.TrimSuffix(name, filepath.Ext(name))
			}
			return strings.ToLower(g.Game + "/" + g.Sub + "/" + name)
		}
		place := key(g.Name)
		if first, ok := taken[place]; ok {
			if g.Game != "Retro" {
				g.Problem = "Another chosen file (" + games[first].File + ") is the same game; only one is used."
				continue
			}
			ext := filepath.Ext(g.Name)
			stem := strings.TrimSuffix(g.Name, ext)
			for k := 2; ; k++ {
				alt := fmt.Sprintf("%s (%d)%s", stem, k, ext)
				if _, used := taken[key(alt)]; !used {
					g.Name = alt
					place = key(alt)
					break
				}
			}
		}
		taken[place] = i
	}
	for i := range games {
		switch games[i].Game {
		case "Retro", "Freedoom", "LibreQuake", "PSP":
			games[i].App = "Game Room"
		}
	}
	sort.SliceStable(games, func(a, b int) bool { return (games[a].Problem == "") && (games[b].Problem != "") })
	return games
}

// bundledCopy reports whether the file is one the installer ships anyway (same size and contents).
func bundledCopy(g *ownGame) bool {
	for _, name := range gameFiles(g.Game) {
		info, err := fs.Stat(bundled, "payload/games/"+g.Game+"/"+name)
		if err != nil || info.Size() != g.Size {
			continue
		}
		want, err := fs.ReadFile(bundled, "payload/games/"+g.Game+"/"+name)
		if err != nil {
			continue
		}
		data, err := g.load()
		if err == nil && bytes.Equal(data, want) {
			return true
		}
	}
	return false
}

// checkedOwnGames returns the last check's usable entries among keys.
func checkedOwnGames(keys []string) []ownGame {
	want := map[string]bool{}
	for _, k := range keys {
		want[k] = true
	}
	ownChecked.Lock()
	defer ownChecked.Unlock()
	var out []ownGame
	for _, g := range ownChecked.list {
		if want[g.Key] && g.Problem == "" {
			out = append(out, g)
		}
	}
	return out
}

// addOwnGames copies the games to their folders on the hard drive (unchanged ones are skipped).
func addOwnGames(r remoteRunner, games []ownGame) error {
	if len(games) == 0 {
		return errors.New("Tick at least one checked game to add.")
	}
	tx, done, err := ownWorkFolder(r)
	if err != nil {
		return err
	}
	defer done()
	groups := map[string][]ownGame{}
	var order []string
	for _, g := range games {
		dir := g.Game + "/" + g.Sub
		if _, ok := groups[dir]; !ok {
			order = append(order, dir)
		}
		groups[dir] = append(groups[dir], g)
	}
	for _, dir := range order {
		if dir == "PSP/yours" {
			var direct, archives []ownGame
			for _, g := range groups[dir] {
				if g.member == "" {
					direct = append(direct, g)
				} else {
					archives = append(archives, g)
				}
			}
			if len(direct) > 0 {
				if err := putPSPGames(r, tx, direct); err != nil {
					return err
				}
			}
			groups[dir] = archives
		}
		var items []putItem
		for _, g := range groups[dir] {
			g := g
			items = append(items, putItem{name: g.Name, load: g.load})
		}
		if err := putGameFiles(r, tx, dir, items); err != nil {
			return err
		}
	}
	return nil
}

// ownWorkFolder makes a working folder holding the file helper (under the install lock), and a function that
// removes it again.
func ownWorkFolder(r remoteRunner) (string, func(), error) {
	if err := claimLock(r); err != nil {
		state, report, inspectErr := inspectInstallLock(r, "")
		if inspectErr == nil && !strings.Contains(report, "Lock type: missing") {
			canUnlock, _ := emptyInstallLock(r)
			return "", nil, &blockedInstall{report: report + "\nEmpty-lock release v2: " + fmt.Sprint(canUnlock), canRecover: state == "staged-only", canUnlockEmpty: canUnlock}
		}
		return "", nil, errors.New("an install is in progress or left a lock; finish or recover it first")
	}
	tx := root + "/" + backupPrefix + fmt.Sprintf("%016x", randomNonce())
	cleanup := func() {
		_, _ = r.run("rm -rf "+shellQuote(tx), nil, 512)
		releaseLock(r)
	}
	if _, err := r.run("umask 077; mkdir "+tx+" && chmod 755 "+tx, nil, 512); err != nil {
		releaseLock(r)
		return "", nil, fmt.Errorf("could not make a working folder: %w", err)
	}
	if err := stageFileOps(r, tx); err != nil {
		cleanup()
		return "", nil, err
	}
	return tx, cleanup, nil
}

// listOwnGames lists the owner's games on the car, as paths under the games folder ("Retro/yours/x.gbc").
func listOwnGames(r remoteRunner) ([]string, error) {
	var script strings.Builder
	fmt.Fprintf(&script, "top=%s\n", shellQuote(gamesRoot))
	for _, d := range ownGameDirs {
		fmt.Fprintf(&script, "for f in \"$top\"/%s/*; do test -f \"$f\" && echo \"${f#$top/}\"; done\n", d)
	}
	script.WriteString("exit 0\n")
	out, err := r.run("/bin/sh -s", []byte(script.String()), 1<<20)
	if err != nil {
		return nil, err
	}
	var list []string
	for _, line := range strings.Split(strings.TrimSpace(string(out)), "\n") {
		if line = strings.TrimSpace(line); line != "" && ownGamePathOK(line) {
			list = append(list, line)
		}
	}
	return list, nil
}

// ownGamePathOK accepts only a plain file directly in one of ownGameDirs.
func ownGamePathOK(p string) bool {
	for _, d := range ownGameDirs {
		if name, ok := strings.CutPrefix(p, d+"/"); ok {
			return name != "" && name != "." && name != ".." && !strings.ContainsAny(name, "/\x00\n")
		}
	}
	return false
}

// removeOwnGames deletes the chosen own games from the hard drive, in one writable window.
func removeOwnGames(r remoteRunner, paths []string) (int, error) {
	if len(paths) == 0 {
		return 0, errors.New("Tick at least one game on the car to remove.")
	}
	var body strings.Builder
	fmt.Fprintf(&body, `mnt=%s
top=%s
made_rw=0
restore() { if test "$made_rw" = 1; then mount -u -o ro "$mnt" || echo "WARNING: $mnt could not be set back to read-only; restarting iDrive does that." >&2; fi; }
trap restore 0
trap 'exit 1' HUP INT TERM
test -d "$mnt" || { echo "The hard drive ($mnt) is not mounted." >&2; exit 1; }
if ! test -w "$mnt"; then mount -u -o rw "$mnt" || { echo "Could not make the hard drive writable." >&2; exit 1; }; made_rw=1; fi
n=0
`, shellQuote(gamesMount), shellQuote(gamesRoot))
	for _, p := range paths {
		if !ownGamePathOK(p) {
			return 0, fmt.Errorf("%q is not one of your games", p)
		}
		fmt.Fprintf(&body, "if test -f \"$top\"/%[1]s; then rm -f \"$top\"/%[1]s && n=$((n+1)); fi\n", shellQuote(p))
	}
	body.WriteString("echo \"removed=$n\"\n")
	out, err := runMutation(r, "/bin/sh -s", []byte(body.String()), 1024)
	if err != nil {
		return 0, err
	}
	n := 0
	fmt.Sscanf(strings.TrimSpace(string(out)), "removed=%d", &n)
	return n, nil
}

// ownGamesAction runs Add / Show / Remove for the page and always returns the list on the car.
func ownGamesAction(r remoteRunner, action string, in vehicleInput) inspection {
	result := inspection{CarGamesListed: true}
	gameNotes = nil
	switch action {
	case "addgames":
		games := checkedOwnGames(in.OwnGames)
		if err := addOwnGames(r, games); err != nil {
			result.Result = "Adding your games stopped. " + err.Error()
			var blocked *blockedInstall
			if errors.As(err, &blocked) {
				result.CanRecover = blocked.canRecover
				result.CanUnlockEmpty = blocked.canUnlockEmpty
			}
		} else {
			names := make([]string, len(games))
			for i, g := range games {
				names[i] = g.Title + " (" + g.App + ")"
			}
			result.Result = "Added to the car: " + strings.Join(names, ", ") + ". They appear in each game's list (marked *) next time you press Play; no iDrive restart needed."
		}
	case "deletestates":
		before := flashStatus(r)
		n, err := deleteSaveStates(r)
		switch {
		case err != nil:
			result.Result = "Deleting save states stopped. " + err.Error()
		case n == 0:
			result.Result = "There were no save states on the car."
		default:
			result.Result = fmt.Sprintf("Deleted %d save state(s) (about %.1f MB). Memory cards and in-game saves were kept.", n, before.statesMB)
		}
	case "removegames":
		n, err := removeOwnGames(r, in.RemoveGames)
		if err != nil {
			result.Result = "Removing stopped. " + err.Error()
		} else {
			result.Result = fmt.Sprintf("Removed %d game file(s) from the car's hard drive. Saves stay on the car.", n)
		}
	}
	list, err := listOwnGames(r)
	if err != nil {
		result.Result = strings.TrimSpace(result.Result + "\nCould not list your games on the car: " + err.Error())
	} else {
		result.CarGames = list
		if action == "listgames" {
			if len(list) == 0 {
				result.Result = "None of your own games are on the car yet."
			} else {
				result.Result = fmt.Sprintf("%d of your own game file(s) are on the car.", len(list))
			}
		}
	}
	if len(gameNotes) > 0 {
		result.Result += "\n" + strings.Join(gameNotes, "\n")
	}
	result.Result = strings.TrimSpace(result.Result + "\n" + flashLine(flashStatus(r)))
	return result
}

func randomNonce() uint64 {
	var b [8]byte
	_, _ = cryptorand.Read(b[:])
	return binary.LittleEndian.Uint64(b[:])
}

// ps1BiosNames are the PlayStation BIOS dumps PCSX ReARMed knows by name (MD5 of the 512 KB file -> its name).
var ps1BiosNames = map[string]string{
	"924e392ed05558ffdb115408c263dccf": "scph1001", "8dd7d5296a650fac7319bce665a6a53c": "scph5500",
	"490f666e1afb15b7362b406ed1cea246": "scph5501", "32736f17079d0b2b7024407c39bd3050": "scph5502",
	"1e68c231d0896b7eadcad1d7d8e76129": "scph7001", "6e3735ff4c7dc899ee98981385f6f3d0": "scph101",
}

// ps1Bios recognises a PlayStation BIOS by its contents.
func ps1Bios(r io.ReaderAt) (string, bool) {
	data := make([]byte, 512*1024)
	if _, err := r.ReadAt(data, 0); err != nil && !errors.Is(err, io.EOF) {
		return "", false
	}
	sum := md5.Sum(data)
	name, ok := ps1BiosNames[fmt.Sprintf("%x", sum)]
	return name, ok
}
