"""Generate the minimal TextDBFile zip for Game Room (plugin/GameRoom_ALL_Texts.zip).

Game Room uses inline text="..." attributes in their XML, so no text IDs are
ever actually looked up from these files - the content doesn't matter. What
matters is that a TextDBFile parameter and a validly-structured zip exist at
all, matching the shape of BMW's own working RHMI plugins
provides, instead of omitting the parameter entirely.
"""
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parents[1] / "plugin"

# locale => display name, matching the set BMW's own plugins ship.
LOCALES = {
    "en-US": "English (US)",
    "en-UK": "English (UK)",
    "de": "Deutsch",
    "es-ES": "Espanol (ES)",
    "es-US": "Espanol (US)",
    "fr-FR": "Francais (FR)",
    "fr-CA": "Francais (CA)",
    "it": "Italiano",
    "pt": "Portugues",
    "ru": "Russkiy",
}


def build(name):
    with ZipFile(ROOT / f"{name}_ALL_Texts.zip", "w", ZIP_DEFLATED) as archive:
        for locale, display in LOCALES.items():
            archive.writestr(ZipInfo(f"{locale}.txt"), f"100000={display}\n", ZIP_DEFLATED)


if __name__ == "__main__":
    build("GameRoom")
