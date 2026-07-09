# -*- mode: python ; coding: utf-8 -*-
# PyInstaller spec for the PTHSonde desktop app.
#   Build from the processor/ directory:  py -3.13 -m PyInstaller PTHSonde.spec
# Produces dist/PTHSonde/PTHSonde.exe (onedir).
#
# SHARPpy is NOT bundled (heavy, separate venv). At runtime the app shells out to
# venv310 if it can find it (next to the exe, or in the dev tree); otherwise it
# falls back to the built-in matplotlib Skew-T panel.
import os
from PyInstaller.utils.hooks import collect_all

proc = os.path.abspath(os.getcwd())              # processor/
root = os.path.dirname(proc)                      # project root

datas = [
    (os.path.join(root, "Dashboard"), "Dashboard"),   # the HTML UI + truck.png
    (os.path.join(proc, "sharppy_render.py"), "."),    # used by the SHARPpy subprocess
]
binaries = []
hiddenimports = ["serial", "serial.tools.list_ports"]

# Pull in everything pywebview needs (Windows EdgeChromium backend, clr, bottle).
for pkg in ("webview",):
    d, b, h = collect_all(pkg)
    datas += d; binaries += b; hiddenimports += h

a = Analysis(
    ["app.py"],
    pathex=[proc],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    runtime_hooks=[],
    excludes=["tkinter", "matplotlib.tests", "numpy.tests"],
    noarchive=False,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz, a.scripts, [],
    exclude_binaries=True,
    name="PTHSonde",
    console=False,                # no console window
    icon=os.path.join(proc, "truck.ico"),   # flying-truck app icon
)
coll = COLLECT(exe, a.binaries, a.datas, name="PTHSonde")
