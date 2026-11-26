"""Game Room app (every game: Freedoom, Tyrian, Quake, NES, Game Boy, PlayStation): layout, manifest, images (run from native/).

The page is a launcher: Play Phase 1, Play Phase 2, Continue last save and the pad layout, plus the hover preview picture
(art/previews/GameRoom.png). The game draws on its own screen layer
(src/freedoom_screen.h), so the page itself never changes (no model writes at all)."""
from io import BytesIO
from pathlib import Path
from zipfile import ZIP_STORED, ZipFile, ZipInfo

from xml.etree import ElementTree as ET

from PIL import Image

import preview

PREVIEW_IMAGE = 57801   # the My Vehicle hover picture (art/previews/GameRoom.png)

plug = Path("plugin")

XML = """<?xml version='1.0' encoding='utf-8'?>
<pluginApps>
  <pluginApp applicationType="VehicleInformation" applicationWeight="220" text="Game Room">
    <entryButton id="1" action="10">
      <properties>
        <property id="23" value="2" />
      </properties>
    </entryButton>
    <actions>
      <hmiAction id="10" target="100" />
      <raAction id="30" />
      <raAction id="31" />
    </actions>
    <hmiStates>
      <hmiState id="100" text="Game Room">
        <components>
          <button id="220" action="30" text="Play (choose a game; wired Xbox pad in the USB port)" />
          <button id="221" action="31" text="Continue last save" />
          <label id="200" text="Freedoom, Tyrian, Quake, NES, Game Boy and PlayStation. Pick a game with the d-pad and A; B leaves the list." />
          <label id="201" text="PC games: left stick moves, RT fires, Menu opens the game's menu (save, load, quit). Questions: A yes, B no." />
          <label id="202" text="Consoles: A / B / X / Y are the console's buttons, Menu Start, View Select. Xbox button: save state, load, other game." />
          <label id="203" text="BACK on the iDrive controller ends the game." />
        </components>
      </hmiState>
    </hmiStates>
  </pluginApp>
</pluginApps>
"""

MANIFEST = """<?xml version='1.0' encoding='utf-8'?>
<plugin>
  <name>GameRoom</name>
  <type>App</type>
  <file>{pluginDir}/GameRoom</file>
  <config>
    <autoStart>yes</autoStart>
    <dependencies>
      <pluginAppConnector>
        <type>Etch</type>
        <id>220</id>
        <asyncCallbacks>yes</asyncCallbacks>
      </pluginAppConnector>
    </dependencies>
    <parameters>
      <DescFile>{pluginDir}/GameRoom.xml</DescFile>
      <ImageDBFile>{pluginDir}/GameRoom_ALL_Images.zip</ImageDBFile>
      <TextDBFile>{pluginDir}/GameRoom_ALL_Texts.zip</TextDBFile>
    </parameters>
  </config>
</plugin>"""


def crlf(text):
    return text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")


doc = ET.ElementTree(ET.fromstring(XML))
app = doc.getroot().find("pluginApp")
if app.find("models") is None:
    models = ET.Element("models")
    app.insert(list(app).index(app.find("hmiStates")), models)
preview.add(app, PREVIEW_IMAGE, "Game Room")
ET.indent(doc, space="  ")
text = "<?xml version='1.0' encoding='utf-8'?>\n" + ET.tostring(doc.getroot(), encoding="unicode") + "\n"
(plug / "GameRoom.xml").write_bytes(crlf(text))
(plug / "GameRoom.manifest").write_bytes(crlf(MANIFEST))
# The page itself uses no images: 57800 is a blank placeholder; 57801 is the hover preview picture.
with ZipFile(plug / "GameRoom_ALL_Images.zip", "w") as archive:
    data = BytesIO()
    Image.new("RGBA", (1, 1)).save(data, format="PNG", optimize=True)
    archive.writestr(ZipInfo("57800.png"), data.getvalue(), ZIP_STORED)
    archive.writestr(ZipInfo(f"{PREVIEW_IMAGE}.png"), preview.png(preview.picture("GameRoom", lambda: None)), ZIP_STORED)
print("ok")
