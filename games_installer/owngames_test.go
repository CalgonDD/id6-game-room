package main

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// fakeGB builds a Game Boy cartridge header the checks accept (color: the CGB flag).
func fakeGB(color bool) []byte {
	rom := make([]byte, 0x8000)
	copy(rom[0x104:], gbLogo)
	copy(rom[0x134:], "TESTGAME")
	if color {
		rom[0x143] = 0x80
	}
	sum := byte(0)
	for _, b := range rom[0x134:0x14D] {
		sum = sum - b - 1
	}
	rom[0x14D] = sum
	return rom
}

func fakeWad(kind string, lumps ...string) []byte {
	var b bytes.Buffer
	b.WriteString(kind)
	binary.Write(&b, binary.LittleEndian, int32(len(lumps)))
	binary.Write(&b, binary.LittleEndian, int32(12))
	for _, l := range lumps {
		binary.Write(&b, binary.LittleEndian, int32(0))
		binary.Write(&b, binary.LittleEndian, int32(0))
		name := make([]byte, 8)
		copy(name, l)
		b.Write(name)
	}
	return b.Bytes()
}

func fakePak(files ...string) []byte {
	var b bytes.Buffer
	b.WriteString("PACK")
	binary.Write(&b, binary.LittleEndian, int32(12))
	binary.Write(&b, binary.LittleEndian, int32(64*len(files)))
	for _, f := range files {
		name := make([]byte, 56)
		copy(name, f)
		b.Write(name)
		binary.Write(&b, binary.LittleEndian, int32(0))
		binary.Write(&b, binary.LittleEndian, int32(0))
	}
	return b.Bytes()
}

func classified(name string, data []byte) ownGame {
	g := ownGame{File: name, Size: int64(len(data))}
	classifyOwnGame(&g, name, bytes.NewReader(data), int64(len(data)))
	return g
}

func TestOwnGamesAreCheckedByWhatIsInside(t *testing.T) {
	nes := append([]byte("NES\x1a\x02\x01"), make([]byte, 16+2*16384+8192)...)
	cases := []struct {
		file, game, sub, name, problem string
	}{
		{"Super Mario Bros. Deluxe (USA, Europe).gbc", "Retro", "yours", "Super Mario Bros. Deluxe (USA, Europe).gbc", ""},
		{"renamed.bin", "Retro", "yours", "renamed.gbc", ""}, // the header, not the name, decides
		{"Tetris.gb", "Retro", "yours", "Tetris.gb", ""},
		{"Zelda: Link?.nes", "Retro", "yours", "Zelda_ Link_.nes", ""},
		{"DOOM2.WAD", "Freedoom", "yours", "doom2.wad", ""},
		{"Final Doom - TNT.wad", "Freedoom", "yours", "tnt.wad", ""},
		{"ultimate.wad", "Freedoom", "yours", "doom.wad", ""},
		{"freedoom2.wad", "", "", "", "already included"},
		{"heretic.wad", "", "", "", "Heretic"},
		{"mymod.wad", "", "", "", "PWAD"},
		{"pak0.pak", "LibreQuake", "yours/id1", "pak0.pak", ""},
		{"PAK1.PAK", "LibreQuake", "yours/id1", "pak1.pak", ""},
		{"hipnotic.pak", "LibreQuake", "yours/hipnotic", "pak0.pak", ""},
		{"mod.pak", "", "", "", "mods"},
		{"Sonic.md", "", "", "", "Mega Drive"},
		{"fake.gbc", "", "", "", "isn't one inside"},
		{"notes.txt", "", "", "", "Not a game file"},
	}
	data := map[string][]byte{
		"Super Mario Bros. Deluxe (USA, Europe).gbc": fakeGB(true),
		"renamed.bin":          fakeGB(true),
		"Tetris.gb":            fakeGB(false),
		"Zelda: Link?.nes":     nes,
		"DOOM2.WAD":            fakeWad("IWAD", "PLAYPAL", "ENDOOM", "MAP01"),
		"Final Doom - TNT.wad": fakeWad("IWAD", "PLAYPAL", "ENDOOM", "MAP01"),
		"ultimate.wad":         fakeWad("IWAD", "PLAYPAL", "ENDOOM", "E1M1", "E2M1", "E4M1"),
		"freedoom2.wad":        fakeWad("IWAD", "FREEDOOM", "PLAYPAL", "MAP01"),
		"heretic.wad":          fakeWad("IWAD", "PLAYPAL", "ENDTEXT", "E1M1"),
		"mymod.wad":            fakeWad("PWAD", "MAP01"),
		"pak0.pak":             fakePak("progs.dat", "maps/start.bsp", "maps/e1m1.bsp", "maps/e2m1.bsp"),
		"PAK1.PAK":             fakePak("maps/e2m1.bsp", "maps/e4m1.bsp"),
		"hipnotic.pak":         fakePak("progs.dat", "maps/hip1m1.bsp"),
		"mod.pak":              fakePak("progs.dat", "maps/mymap.bsp"),
		"Sonic.md":             append(make([]byte, 0x100), []byte("SEGA MEGA DRIVE ")...),
		"fake.gbc":             []byte("this is not a game boy cartridge at all, just text that is long enough"),
		"notes.txt":            []byte("hello"),
	}
	for _, c := range cases {
		g := classified(c.file, data[c.file])
		if c.problem != "" {
			if g.Problem == "" || !strings.Contains(g.Problem, c.problem) {
				t.Errorf("%s: want a problem with %q, got %+v", c.file, c.problem, g)
			}
			continue
		}
		if g.Problem != "" || g.Game != c.game || g.Sub != c.sub || g.Name != c.name {
			t.Errorf("%s: got game=%q sub=%q name=%q problem=%q", c.file, g.Game, g.Sub, g.Name, g.Problem)
		}
	}
	// the real bundled homebrew passes the header checks
	for _, f := range []string{"../third_party/gb_homebrew/uCity.gbc", "../third_party/gb_homebrew/Libbet and the Magic Floor.gb", "../third_party/nes_homebrew/Thwaite.nes"} {
		raw, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		if g := classified(filepath.Base(f), raw); g.Problem != "" || g.Game != "Retro" {
			t.Errorf("%s: %+v", f, g)
		}
	}
}

func TestOwnGamesFromFoldersAndZips(t *testing.T) {
	dir := t.TempDir()
	write := func(name string, data []byte) {
		if err := os.WriteFile(filepath.Join(dir, name), data, 0644); err != nil {
			t.Fatal(err)
		}
	}
	write("Game A.gbc", fakeGB(true))
	write("readme.txt", []byte("not a game"))
	var z bytes.Buffer
	zw := zip.NewWriter(&z)
	for name, data := range map[string][]byte{"inside/Game A.gbc": fakeGB(false), "inside/info.txt": []byte("x")} {
		w, _ := zw.Create(name)
		w.Write(data)
	}
	zw.Close()
	write("pack.zip", z.Bytes())
	games, skipped := checkOwnGames([]string{dir})
	if skipped != 1 {
		t.Fatalf("readme.txt should be skipped quietly, skipped=%d %+v", skipped, games)
	}
	var names []string
	for _, g := range games {
		if g.Problem != "" {
			t.Fatalf("unexpected problem: %+v", g)
		}
		names = append(names, g.Name)
	}
	// both are "Game A": the second keeps its own name with (2)
	if strings.Join(names, "|") != "Game A.gbc|Game A (2).gb" && strings.Join(names, "|") != "Game A.gb|Game A (2).gbc" {
		t.Fatalf("names: %v", names)
	}
	for _, g := range games {
		data, err := g.load()
		if err != nil || len(data) != 0x8000 {
			t.Fatalf("load %s: %d %v", g.File, len(data), err)
		}
	}
	if got := checkedOwnGames([]string{games[0].Key}); len(got) != 1 || got[0].Key != games[0].Key {
		t.Fatalf("checkedOwnGames: %+v", got)
	}
}

// End to end on a real shell: add (names with spaces), list, uninstall keeps them, remove.
func TestOwnGamesGoOnTheHardDriveAndSurviveUninstall(t *testing.T) {
	dir, restore := useTestDrives(t)
	defer restore()
	f := gamesFixture{shellFixture: shellFixture{dir: dir}, mountLog: filepath.Join(dir, "mount.log"), readOnly: true}
	pc := t.TempDir()
	gb := filepath.Join(pc, "Super Mario Bros. Deluxe (USA, Europe).gbc")
	if err := os.WriteFile(gb, fakeGB(true), 0644); err != nil {
		t.Fatal(err)
	}
	pak := filepath.Join(pc, "pak0.pak")
	if err := os.WriteFile(pak, fakePak("progs.dat", "maps/start.bsp", "maps/e1m1.bsp"), 0644); err != nil {
		t.Fatal(err)
	}
	games, _ := checkOwnGames([]string{gb, pak})
	keys := []string{games[0].Key, games[1].Key}
	result := ownGamesAction(f, "addgames", vehicleInput{OwnGames: keys})
	if !strings.Contains(result.Result, "Added to the car") || len(result.CarGames) != 2 {
		t.Fatalf("add: %q %v", result.Result, result.CarGames)
	}
	got, err := os.ReadFile(filepath.Join(dir, "mntdata", "ID6Games", "Retro", "yours", "Super Mario Bros. Deluxe (USA, Europe).gbc"))
	if err != nil || !bytes.Equal(got, fakeGB(true)) {
		t.Fatalf("the game is not on the hard drive: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "mntdata", "ID6Games", "LibreQuake", "yours", "id1", "pak0.pak")); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(f.mountLog + ".rw"); !os.IsNotExist(err) {
		t.Fatal("the hard drive was left writable")
	}
	if entries, _ := os.ReadDir(filepath.Join(dir, "mntshare", "ID6Games-staging")); len(entries) != 0 {
		t.Fatalf("staging left behind: %v", entries)
	}
	if left, _ := filepath.Glob(filepath.Join(dir, backupPrefix+"*")); len(left) != 0 {
		t.Fatalf("working folder left behind: %v", left)
	}
	// adding again uploads nothing
	result = ownGamesAction(f, "addgames", vehicleInput{OwnGames: keys})
	if !strings.Contains(result.Result, "unchanged") {
		t.Fatalf("second add: %q", result.Result)
	}
	// uninstalling Retro keeps the owner's games
	os.WriteFile(filepath.Join(dir, "mntdata", "ID6Games", "Retro", "Thwaite.nes"), []byte("bundled"), 0644)
	gameNotes = nil
	if err := removeGameData(f, "Retro"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, "mntdata", "ID6Games", "Retro", "Thwaite.nes")); !os.IsNotExist(err) {
		t.Fatal("the bundled game was not removed")
	}
	list, err := listOwnGames(f)
	if err != nil || len(list) != 2 || list[0] != "Retro/yours/Super Mario Bros. Deluxe (USA, Europe).gbc" || list[1] != "LibreQuake/yours/id1/pak0.pak" {
		t.Fatalf("after uninstall: %v %v", list, err)
	}
	// remove refuses anything outside the yours folders, then removes the chosen one
	if _, err := removeOwnGames(f, []string{"Retro/Thwaite.nes"}); err == nil {
		t.Fatal("a bundled file must not be removable here")
	}
	if _, err := removeOwnGames(f, []string{"Retro/yours/../x"}); err == nil {
		t.Fatal("a path with .. must be refused")
	}
	result = ownGamesAction(f, "removegames", vehicleInput{RemoveGames: []string{list[0]}})
	if !strings.Contains(result.Result, "Removed 1") || len(result.CarGames) != 1 {
		t.Fatalf("remove: %q %v", result.Result, result.CarGames)
	}
	if _, err := os.Stat(f.mountLog + ".rw"); !os.IsNotExist(err) {
		t.Fatal("the hard drive was left writable after removing")
	}
}

// The bundled game files with spaces in their names upload too (they used to be staged under their own names).
func TestBundledGameFilesWithSpacesInstall(t *testing.T) {
	dir, restore := useTestDrives(t)
	defer restore()
	f := gamesFixture{shellFixture: shellFixture{dir: dir}, mountLog: filepath.Join(dir, "mount.log")}
	tx := testShellPath(filepath.Join(dir, "tx"))
	gameNotes = nil
	items := []putItem{{name: "Nova the Squirrel.nes", load: func() ([]byte, error) { return []byte("nova"), nil }}}
	if err := putGameFiles(f, tx, "Retro", items); err != nil {
		t.Fatal(err)
	}
	if got, err := os.ReadFile(filepath.Join(dir, "mntdata", "ID6Games", "Retro", "Nova the Squirrel.nes")); err != nil || string(got) != "nova" {
		t.Fatalf("%q %v", got, err)
	}
}

func TestPlayStationDiscsAndBios(t *testing.T) {
	chd := append([]byte("MComprHD\x00\x00\x00\x7c\x00\x00\x00\x05"), make([]byte, 4096)...)
	if g := classified("Gran Turismo 2 (USA) (Disc 1).chd", chd); g.Problem != "" || g.Game != "Retro" || g.Name != "Gran Turismo 2 (USA) (Disc 1).chd" {
		t.Fatalf("chd: %+v", g)
	}
	if g := classified("fake.chd", []byte("not a disc image at all, just some text that is long enough")); !strings.Contains(g.Problem, "isn't one inside") {
		t.Fatalf("fake chd: %+v", g)
	}
	if g := classified("Game (Track 1).bin", make([]byte, 512*1024)); !strings.Contains(g.Problem, "convert it to .chd") {
		t.Fatalf("a .bin that is no BIOS: %+v", g)
	}
	bios, err := os.ReadFile("../private/bios/scph1001.bin")
	if err != nil {
		t.Skip("no private BIOS copy to check against")
	}
	if g := classified("SCPH1001.BIN", bios); g.Problem != "" || g.Game != "Retro" || g.Sub != "yours" || g.Name != "scph1001.bin" {
		t.Fatalf("bios: %+v", g)
	}
}
