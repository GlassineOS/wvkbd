wvkbd(1)

# NAME

wvkbd - on-screen virtual keyboard for wayland compositors using wlroots

# SYNOPSIS

wvkbd-mobintl [OPTIONS]...
wvkbd-deskintl [OPTIONS]...

*NOTE*: Your binary may have a different suffix depending on which layout you compiled.

# DESCRIPTION

This project aims to deliver a minimal but practically usable implementation of
a wlroots on-screen keyboard in legible C. This will _only_ be a keyboard, not
a feedback buzzer, led blinker, or anything that requires more than what's
needed to input text quickly. The end product should be a static codebase that
can be patched to add new features.

## OPTIONS

*-D*
	enable debug mode.

*-o*
	print pressed keys to standard output.

*-O*
	print intersected keys to standard output.

*-l* _layers_
	comma separated list of layers in vertical/portrait mode.

*--landscape-layers* _layers_
	comma separated list of layers used in horizontal/landscape mode.

*--list-layers*
	prints a list of all available layers.

*-H* _pixels_
	Height of the keyboard in pixels, for vertical/portrait mode.

*-L* _pixels_
	Height of the keyboard in pixels, for horizontal/landscape mode

*-R* _pixels_
	Rounding radius of the keys in pixels.

*--fn* _font_
	set font and size (e.g. DejaVu Sans 20)

*--hidden*
	Start hidden (send SIGUSR2 to show).

*--hold* _ms_
	Long press: a key that has an alternate (printed small in its top right
	corner) types it when held for _ms_ milliseconds; released sooner, it types
	its own character. Keys without an alternate are unchanged. Default 400;
	0 turns it off. The letter keys and *,* and *.* of the full, landscape and
	German layers have alternates.

*--no-popup*
	Disable the key-press popup (the magnified key shown above a key while
	it is held). Useful for privacy, e.g. when typing on a lockscreen.

*--no-highlight*
	Don't highlight a key while it is pressed (draw it the same as an
	unpressed key). Useful for privacy, e.g. when typing on a lockscreen.

*--no-feedback*
	Disable all visual key-press feedback; shorthand for *--no-popup* and
	*--no-highlight* together.

*--non-exclusive*
	Allow keyboard to overlap existing windows, do not request an
	exclusive zone from the compositor.

*--wayland-layer* _[background|bottom|top|overlay]_
	Wayland layer shell z index

*--dock* _left|right|full_
	Dock keyboard to the left or right screen corner for comfortable one-handed
	typing on tablets and foldables. Defaults to full.

*--ratio* _float_
	Width ratio when docked [0.3-1.0]. Defaults to 0.68.

*--width* _int_
	Explicit custom width in pixels.

*--corner-radius* _int_
	Corner radius in pixels for the keyboard window background (defaults to 16 when docked,
	0 when full-width).

*--bg-alpha* _int_
	Set independent alpha value for the keyboard baseplate [0-255]. Allows
	semi-opaque or opaque backing while maintaining translucent keycaps.

*--alpha* _int_
	Set alpha value (i.e. transparency) for all colors [0-255]
	
*--auto*
	Automatically toggle keyboard visibility based on focus. Requires a compositor
	that implements the zwp_input_method_v2 protocol. Without *--im* the keyboard
	only shows and hides; with it, see *INPUT METHOD*.

*--im*
	Be the input method: with a text field focused, text is committed to the
	field, Backspace deletes through the input method where the field reports
	its text, and other keys stay key events. The field's purpose picks the
	keys (number, phone and PIN pads; an *@* row for email, a */* and *.com* row
	for web addresses). See *INPUT METHOD*.

*--strip*
	With *--im*: a suggestion strip above the keys, with up to three words
	(the likeliest in the middle), a pen key (*--pen-cmd*) and a hide key.

*--strip-height* _pixels_
	Height of the suggestion strip. Defaults to the height of one key row.

*--predictor* _command_
	The word predictor for the strip: _command_ is split at spaces (no shell)
	and started as a child process whose standard input and output are one
	end of a socket pair, for the predictor line contract (docs/predictor.md
	of GlassineOS/shell). Missing, slow or crashed: the strip stays empty and
	typing goes on; it is started again with a growing delay.

*--lang* _languages_
	Comma separated list of the languages sent to the predictor. Defaults to
	the language of the active layout.

*--notify* _socket_
	Send focus reports to this Unix datagram socket, one line each, never
	waiting for it: *osk focus 1* _purpose_ when a text field gets the focus
	(_purpose_ is the field's text-input content purpose as a number),
	*osk focus 0* when it loses it, *osk changed* at each other report of the
	focused field (its text, cursor or type), and *osk im* when the seat
	already has another input method.

*--hide-cmd* _command_
	The strip's hide key runs this (with _sh -c_) instead of hiding the
	keyboard itself.

*--pen-cmd* _command_
	The strip's pen key (handwriting) runs this (with _sh -c_). Without it
	there is no pen key.
	
*--bg* _rrggbb|aa_ 
	Set color of background

*--fg* _rrggbb|aa_       
	Set color of keys

*--fg-sp* _rrggbb|aa_       
	Set color of special keys

*--press* _rrggbb|aa_       
	Set color of pressed keys

*--press-sp* _rrggbb|aa_       
	Set color of pressed special keys

*--swipe* _rrggbb|aa_       
	Set color of swiped keys

*--swipe-sp* _rrggbb|aa_       
	Set color of swiped special keys

*--text* _rrggbb|aa_       
	Set color text on keys

*--text-sp* _rrggbb|aa_       
	Set color text on special keys

*--text-press* _rrggbb|aa_
	Set color of text on pressed keys

*--text-press-sp* _rrggbb|aa_
	Set color of text on pressed special keys

*--text-swipe* _rrggbb|aa_
	Set color of text on swiped keys. Defaults to the *--text* color.

*--text-swipe-sp* _rrggbb|aa_
	Set color of text on swiped special keys

*--stroke* _rrggbb|aa_
	Set color of the outline of keys

*--stroke-sp* _rrggbb|aa_
	Set color of the outline of special keys

*--stroke-width* _pixels_
	Width of the key outline in pixels.

*--gap* _pixels_
	Space around each key in pixels. Defaults to 2.

*--version*
	Print version information

*-h*, *--help*
	Print usage help 


# INPUT METHOD

With *--im*, the keyboard is the seat's input method (zwp_input_method_v2).
Only one input method per seat is possible: when another one holds the seat, the
keys are plain key events and the keyboard tries again later (and reports
*osk im* to *--notify*).

Private fields (purpose password or PIN, or the sensitive data or hidden text
hint) send nothing to the predictor, and with *-o* print nothing on standard
output. The strip shows a lock in them and no pen key.

A word is learned when it is finished (a space, punctuation or Enter after it)
or picked from the strip, once: finishing the same word at the same place again
(after deleting the space after it) learns nothing more, until another key or a
change the field makes by itself. Holding a word in the
strip asks whether to remove it.

# FILES

_$XDG_CONFIG_HOME/glassine/predict.conf_ (or _~/.config/glassine/predict.conf_)
	*suggestions = off* leaves the strip empty and sends nothing to the
	predictor; *handwriting = off* hides the pen key (*off*, *false* or *0*;
	anything else, or no line, is on). Read again at each field focus and
	each show.

# SIGNALS

You can send signals to wvkbd to hide/show it (e.g. using _kill_(1) with _-s_):

*SIGUSR1*
	Hide the keyboard.

*SIGUSR2*
	Show the keyboard

*SIGRTMIN*
	Toggle visibility

# COMPOSE BUTTON

The default mobile international layout features a Compose button (*Cmp*)
which, when combined with another key, opens up a layout that offers variants
for that key. This is similar to functionality that other keyboards implemented
using a *long press* (wvkbd has no such notion, holding a key will repeat
it like on a physical keyboard).

For example, press Cmp + a to access variants with diacritics like á,à,â,ä, etc.. 

Most layouts also feature the following that are less obvious:

- Press Cmp and . to access more punctuation
- Press Cmp and - or , to access 'mathematical' symbols (+,-,=,etc)
- Press Cmp and ' or 0 or 9 to access more brackets and quotes
- Press Cmp and q to access emojis

Last, but not least, pressing Cmp + space or Cmp + ⌨  or Cmp + Abc opens up an
index that allows you to immediately jump to any layout by name, even layouts
not explicitly added to your layers on startup.

# AUTHORS

Created by John Sullivan <jsullivan@csumb.edu>, maintained by the Sxmo project
<https://sxmo.org> in collaboration with other open source contributors. For
more information about wvkbd development, see <https://git.sr.ht/~proycon/wvkbd>
or <https://github.com/jjsullivan5196/wvkbd>.
