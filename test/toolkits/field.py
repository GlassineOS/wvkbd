#!/usr/bin/env python3
# A real toolkit's text field for the toolkit test: prints "text BUFFER|CURSOR" on every
# change. field.py 3|4 [entry|view]
import sys
import gi
v = sys.argv[1]
kind = sys.argv[2] if len(sys.argv) > 2 else "entry"
gi.require_version("Gtk", v + ".0")
from gi.repository import Gtk, GLib

if kind == "view":
    e = Gtk.TextView()
    b = e.get_buffer()

    def show(*a):
        it = b.get_iter_at_mark(b.get_insert())
        t = b.get_text(b.get_start_iter(), b.get_end_iter(), True)
        print("text " + t + "|" + str(it.get_offset()), flush=True)
    b.connect("changed", show)
    b.connect("notify::cursor-position", show)
else:
    e = Gtk.Entry()

    def show(*a):
        print("text " + e.get_text() + "|" + str(e.get_position()), flush=True)
    e.connect("changed", show)
    e.connect("notify::cursor-position", show)

if v == "3":
    w = Gtk.Window()
    w.add(e)
    w.connect("destroy", Gtk.main_quit)
    w.show_all()
    e.grab_focus()
    Gtk.main()
else:
    loop = GLib.MainLoop()
    w = Gtk.Window()
    w.set_child(e)
    w.connect("close-request", lambda *a: loop.quit())
    w.present()
    e.grab_focus()
    loop.run()
