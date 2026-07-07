#!/usr/bin/env python3
"""
PTHSonde desktop app.

Starts the local server (serial reader + flight processing + dashboard) in a
background thread and shows the dashboard in a native window — one process, no
browser, no separate console. Run with `python app.py`, or build a single
executable with PyInstaller (see build_app.py / the .spec).
"""
import threading, time, os, sys, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sonde_server

HOST, PORT = "127.0.0.1", 8765
URL = "http://%s:%d/" % (HOST, PORT)


def _run_server():
    sonde_server.app.run(host=HOST, port=PORT, threaded=True, use_reloader=False)


def _wait_up(timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        try:
            urllib.request.urlopen(URL + "ping", timeout=0.5)
            return True
        except Exception:
            time.sleep(0.15)
    return False


def main():
    threading.Thread(target=_run_server, daemon=True).start()
    _wait_up()
    import webview
    win = webview.create_window("PTHSonde Ground Station", URL,
                                width=1500, height=950, min_size=(1100, 700))

    def _on_closing():
        try:
            sonde_server._serial_stop()
        except Exception:
            pass

    try:
        win.events.closing += _on_closing
    except Exception:
        pass
    webview.start()


if __name__ == "__main__":
    main()
