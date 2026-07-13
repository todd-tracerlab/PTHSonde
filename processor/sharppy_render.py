"""
Render the REAL SHARPpy SPC panel headless (no network, no Main()/data-sources).
Runs under processor/venv311 (Python 3.11 + SHARPpy git GUI + PySide6).

    python sharppy_render.py <input.json> <output.png>

input.json: {"loc","time"(YYMMDD/HHMM),"lat","lon","levels":[{p,h,t,td,wd,ws}...]}
"""
import os, sys, json
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ["QT_API"] = "pyside2"          # SHARPpy's GUI is Qt5; PySide2 is lenient
# Qt's offscreen platform ships no fonts -> point it at the Windows font dir so
# all the index text / labels actually render.
_winfonts = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")
if os.path.isdir(_winfonts):
    os.environ.setdefault("QT_QPA_FONTDIR", _winfonts)


def main():
    inp, outpng = sys.argv[1], sys.argv[2]
    d = json.load(open(inp, "r", encoding="utf-8"))
    work = os.path.dirname(os.path.abspath(outpng))
    spc = os.path.join(work, "_sounding.txt")
    ini = os.path.join(work, "_sharppy.ini")

    loc = (d.get("loc") or "SONDE").split()[0]
    lines = ["%TITLE%",
             " %s %s %.2f,%.2f" % (loc, d["time"], d["lat"], d["lon"]),
             "", "%RAW%"]
    for lv in d["levels"]:
        lines.append(" %.2f,%.2f,%.2f,%.2f,%.2f,%.2f"
                     % (lv["p"], lv["h"], lv["t"], lv["td"], lv["wd"], lv["ws"]))
    lines.append("%END%")
    with open(spc, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    from qtpy.QtWidgets import QApplication
    app = QApplication.instance() or QApplication([])

    # SHARPpy (Qt5 code) passes floats where PyQt5 wants ints. Coerce QFont args.
    # It also asks for QFont('Helvetica') 59x -- a font that does NOT exist on
    # Windows. Under the offscreen Qt plugin that name gets substituted with a
    # symbol/bitmap font whose lowercase cmap is wrong, so labels render as
    # scrambled shapes ("Storm" -> "St~rm"). Rewrite the missing family to a real
    # Windows sans (Arial) right at construction so every glyph maps correctly.
    from qtpy import QtGui as _G
    _OF = _G.QFont
    _MISSING = {"helvetica", "sans-serif", "sans serif", "sans"}
    def _SafeFont(*a, **k):
        a = list(a)
        if a and isinstance(a[0], str) and a[0].strip().lower() in _MISSING:
            a[0] = "Arial"
        for i in (1, 2, 3):
            if len(a) > i and isinstance(a[i], float):
                a[i] = int(round(a[i]))
        return _OF(*a, **k)
    _G.QFont = _SafeFont
    # Belt-and-suspenders: register the substitution with Qt's font DB too, and
    # make Arial the application default so nothing falls through to a bad font.
    try:
        _OF.insertSubstitutions("Helvetica", ["Arial", "Segoe UI", "Tahoma"])
        app.setFont(_OF("Arial", 10))
    except Exception:
        pass

    from sutils.config import Config
    from sharppy.io.spc_decoder import SPCDecoder
    from sharppy.viz.SPCWindow import SPCWidget
    from sharppy.viz.preferences import PrefDialog

    cfg = Config(ini)
    PrefDialog.initConfig(cfg)                   # default colors/units/insets

    # Zero out UNREALIZABLE CAPE so the panel matches physics (and the text
    # analysis). Under a strong cap SHARPpy sums positive buoyancy slivers with NO
    # LFC -- CAPE that can never be reached. If a parcel has no LFC AND is stable
    # (Lifted Index >= 0), force its CAPE to 0. A negative LI is real instability,
    # so it is left untouched. This patches the function the panel uses to compute
    # every parcel, so the Skew-T fill, the PCL table, and the indices all agree.
    import sharppy.sharptab.params as _params
    _orig_parcelx = _params.parcelx
    def _guarded_parcelx(*a, **k):
        pcl = _orig_parcelx(*a, **k)
        try:
            lfc = float(pcl.lfchght); li = float(pcl.li5)
            if lfc != lfc and li == li and li >= 0.0:     # no LFC + stable -> phantom
                pcl.bplus = 0.0
        except Exception:
            pass
        return pcl
    _params.parcelx = _guarded_parcelx

    prof_coll = SPCDecoder(spc).getProfiles()
    prof_coll.setMeta('run', prof_coll.getMeta('base_time'))
    prof_coll.setMeta('model', 'Observed')
    prof_coll.setMeta('observed', True)

    widget = SPCWidget(cfg=cfg)                  # the panel itself (no menu/parent)
    widget.resize(2460, 1620)                    # 2x native -> crisp, higher DPI
    widget.show()
    widget.addProfileCollection(prof_coll, "obs")
    for _ in range(15):
        app.processEvents()
    widget.pixmapToFile(outpng)

    for tmp in (spc, ini):
        try:
            os.remove(tmp)
        except OSError:
            pass
    ok = os.path.exists(outpng) and os.path.getsize(outpng) > 1000
    print("OK" if ok else "FAIL")
    sys.exit(0 if ok else 2)


if __name__ == "__main__":
    main()
