# PIEG keyboard layouts

Layout files live here in the project and are installed on the PIEG volume as

```
/INPUTDEV/KEYBOARD/   layouts (*.KBD) and KEYMAP.CFG, the saved choice
/INPUTDEV/MOUSE/      empty, reserved for later
/INPUTDEV/CUSTOM/     empty, reserved for later
```

`make run`, `make fsimg` and `make install-programs` copy every
`Keyboard/*.kbd` into `/INPUTDEV/KEYBOARD/` (names become upper case, for
example `de.kbd` becomes `DE.KBD`). The three `/INPUTDEV` folders are also
created at boot when a volume lacks them.

## Layouts

| File | Layout |
|---|---|
| `us.kbd` | US English (QWERTY), same as the built-in default |
| `uk.kbd` | UK English (ISO QWERTY) |
| `de.kbd` | German (QWERTZ) |
| `fr.kbd` | French (AZERTY) |
| `se.kbd` | Swedish (QWERTY) |
| `dvorak.kbd` | Dvorak (US) |
| `colemak.kbd` | Colemak (US) |

## The `keymap` command

```
keymap                 show the current layout and the available ones
keymap de              switch to DE.KBD and keep it for the next boot
keymap -t de           switch until reboot only
keymap /path/x.kbd     load any layout file (not kept after reboot)
keymap --test          type to try the layout, Ctrl+C ends
```

"Kept" means the name is written to `/INPUTDEV/KEYBOARD/KEYMAP.CFG`. The
kernel reads it right after the filesystem mounts, before the "press any
key" screen, so the shell starts with the saved layout. A missing, unreadable
or invalid config leaves the built-in US layout.

AltGr is the right Alt key. Ctrl+letter follows the layout, so on Dvorak the
key in the QWERTY C position gives Ctrl+J.

## Limitations

The terminal, the login prompt and the 5x7 font only handle ASCII
(0x20-0x7E). Layouts therefore cannot type letters such as a-umlaut, e-acute
or a-ring, and the keys that carry them type nothing (`none`). Dead keys are
not supported: `^` and `` ` `` type themselves where a layout has them, and
the acute and diaeresis keys type nothing. The layout files that lose
characters say which ones in their first comment lines. Once the font and the terminal accept
more than ASCII, only the layout files need updating.

## File format

Plain text, one entry per line. Blank lines and lines starting with `#` are
ignored. Comments cannot follow an entry, because `#` is also a key.

```
name German (QWERTZ)
# SC  plain shift altgr
15    z     Z
08    7     /     {
0C    none  ?     \
```

- `name TEXT` is the display name (31 characters, printable ASCII). Without
  it the file name is used.
- `SC plain shift [altgr]` sets one key. `SC` is the Set-1 scancode in hex,
  `01` to `7F`. The AltGr column is optional.
- A key is one printable ASCII character (`a`, `;`, `\`), or `space`,
  `none` (types nothing), `\\`, `\n`, `\b`, or `\xHH` (`00`, `08`, `0A`,
  `20`-`7E`).
- Every layout starts as a copy of US, so a file lists only the keys that
  differ. To remove a character a US key would give, set it to `none`.
- Files are at most 8 KiB. The file name is the layout name: 8 characters or
  fewer plus `.kbd`, for `keymap <name>` and for saving to work.

Common scancodes (US positions):

```
29 `   02-0B 1..0   0C -   0D =   0E Backspace
10-19 q..p   1A [   1B ]   1C Enter
1E-26 a..l   27 ;   28 '   2B \
56 ISO <>|   2C-32 z..m   33 ,   34 .   35 /   39 Space
```

If a file is wrong, `keymap` reports the line, for example
`keymap: de: bad character (printable ASCII, none, space) on line 12`.
A failed load never changes the active layout.

## Adding a layout

1. Copy a similar file, for example `de.kbd`, to `Keyboard/xx.kbd`.
2. Edit the `name` line and the keys.
3. `make host-test` checks that it parses, uses only safe characters, types
   every printable ASCII character and has each letter on exactly one key.
4. `make install-programs`, boot, `keymap xx`, then `keymap --test`.

`/INPUTDEV/CUSTOM` is where user-made layouts will live later. For now,
anywhere works with `keymap <path>`.
