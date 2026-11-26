"""Static hover previews for the iDrive app list.

Without a preview model, iDrive renders an app's real page when the app is only
hovered in the list. That page then counts as "visible", so the app starts its
data worker and sends screen updates while iDrive is at its busiest (list
animation + preview), which is when the HMI freeze was hit. NBT-AAD avoids this
with a StatePreview model pointing at a separate page that holds one 549x484
picture; we do the same. The real page (and the live data) only starts once
the app is actually opened.

IDs used by Game Room's preview:
  model 120 = raPreviewModel (StatePreview -> state 106)
  model 121 = imageIdModel holding the preview picture
  state 106 = preview page, appended LAST so existing state numbers do not move
  component 260 = the picture on that page
"""
from pathlib import Path
from xml.etree import ElementTree as ET

from PIL import Image

PREVIEW_MODEL, PICTURE_MODEL, PREVIEW_STATE, PICTURE_COMPONENT = 120, 121, 106, 260
WIDTH, HEIGHT = 549, 484
BACKGROUND = "#0b0f14"


def add(app, image_id, title):
    """Wire the preview into a <pluginApp> element. Call after all other states exist."""
    app.find("entryButton").set("previewModel", str(PREVIEW_MODEL))
    models = app.find("models")
    ET.SubElement(models, "raPreviewModel", id=str(PREVIEW_MODEL), modelType="StatePreview",
                  previewState=str(PREVIEW_STATE))
    ET.SubElement(models, "imageIdModel", id=str(PICTURE_MODEL), imageId=str(image_id))
    state = ET.SubElement(app.find("hmiStates"), "hmiState", id=str(PREVIEW_STATE), text=title)
    picture = ET.SubElement(ET.SubElement(state, "components"), "image",
                            id=str(PICTURE_COMPONENT), model=str(PICTURE_MODEL))
    properties = ET.SubElement(picture, "properties")
    for prop, value in ((3, "true"), (9, WIDTH), (10, HEIGHT), (20, 0), (21, 0)):
        ET.SubElement(properties, "property", id=str(prop), value=str(value))


CUSTOM = Path(__file__).resolve().parents[1] / "art" / "previews"


def picture(app_name, make_default):
    """The owner's own preview (art/previews/<app_name>.png, any size: it is fitted
    to 549x484 on the dark background) if present, else make_default()."""
    path = CUSTOM / f"{app_name}.png"
    if not path.is_file():
        return make_default()
    own = Image.open(path).convert("RGBA")
    scale = min(WIDTH / own.width, HEIGHT / own.height)
    own = own.resize((max(1, round(own.width * scale)), max(1, round(own.height * scale))), Image.LANCZOS)
    image = Image.new("RGBA", (WIDTH, HEIGHT), BACKGROUND)
    image.paste(own, ((WIDTH - own.width) // 2, (HEIGHT - own.height) // 2), own)
    return image


def png(image):
    """PNG bytes for an images zip."""
    from io import BytesIO
    data = BytesIO()
    image.save(data, format="PNG", optimize=True)
    return data.getvalue()

