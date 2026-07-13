PTHSonde — User Manual (LaTeX / Overleaf)
==========================================

FILES
  main.tex        The full document — copy/paste this into Overleaf.
  main.pdf        A pre-compiled preview (17 pages) so you can see the result.
  assets/         All images the document references. UPLOAD THIS FOLDER to Overleaf.

USING IT IN OVERLEAF
  1. New Project -> Blank Project.
  2. Paste the contents of main.tex over the default main.tex.
  3. Upload the entire assets/ folder (Menu -> Upload, or drag the folder in) so the
     paths like assets/dash_profile.png resolve.
  4. Set the compiler to pdfLaTeX (Menu -> Compiler) and Recompile.

ASSETS USED
  logo_pthsonde.png, logo_tracerlab.png   (title page)
  sonde_battery.png, sonde_ziptie.png, sonde_flightline.png   (sonde prep, backgrounds removed)
  dash_profile.png, dash_map.png, dash_timeseries.png, dash_health.png   (dashboard)
  sharppy_example.png   (SHARPpy sounding)
  launch.jpg            (launch photo)

Everything compiles with standard packages (tikz, tcolorbox, booktabs, xcolor[table], hyperref).
