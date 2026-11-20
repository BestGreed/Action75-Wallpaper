---
name: ACTION75
description: Action75 Windows 控制台的配色、字体与界面层级。
colors:
  paper-highlight: "#f5f5f5"
  paper-ground: "#e6e6e6"
  paper-shade: "#c9c9c9"
  paper-ink: "#202020"
  paper-muted: "#4f4f4f"
  paper-face: "#f9f9f9"
  paper-edge: "#b5b5b5"
  paper-selection: "#d3d3d3"
  paper-trace: "#292929"
  graphite-highlight: "#333333"
  graphite-ground: "#1c1c1c"
  graphite-shade: "#101010"
  graphite-ink: "#f0f0f0"
  graphite-muted: "#c0c0c0"
  graphite-face: "#393939"
  graphite-edge: "#686868"
  graphite-selection: "#4e4e4e"
  graphite-trace: "#e5e5e5"
  paper-error: "#99312a"
  graphite-error: "#f8a49b"
typography:
  display:
    fontFamily: "Action75 Display, Microsoft YaHei, sans-serif"
    fontSize: "36px"
  headline:
    fontFamily: "Action75 Display, Microsoft YaHei, sans-serif"
    fontSize: "31px"
  title:
    fontFamily: "Action75 Display, Microsoft YaHei, sans-serif"
    fontSize: "27px"
  body:
    fontFamily: "Microsoft YaHei, sans-serif"
    fontSize: "14px"
  label:
    fontFamily: "Microsoft YaHei, sans-serif"
    fontSize: "13px"
  small:
    fontFamily: "Microsoft YaHei, sans-serif"
    fontSize: "12px"
  wordmark:
    fontFamily: "Microsoft YaHei, sans-serif"
    fontSize: "13px"
    letterSpacing: "2.2px"
rounded:
  preview: "3px"
  control: "5px"
  theme-key: "4px"
spacing:
  control-gap-x: "9px"
  control-gap-y: "8px"
  navigation-inset-x: "16px"
  navigation-inset-y: "14px"
  page-gutter: "26px"
  gallery-gap: "22px"
  gallery-gap-compact: "18px"
components:
  button:
    backgroundColor: "{colors.paper-face}"
    textColor: "{colors.paper-ink}"
    typography: "{typography.body}"
    rounded: "{rounded.control}"
    padding: "7px 12px"
  button-graphite:
    backgroundColor: "{colors.graphite-face}"
    textColor: "{colors.graphite-ink}"
  search:
    typography: "{typography.body}"
    width: "110px"
  navigation:
    typography: "{typography.label}"
    width: "122px"
  preview:
    rounded: "{rounded.preview}"
    typography: "{typography.label}"
  slider:
    rounded: "2px"
    typography: "{typography.body}"
    height: "12px"
  theme-key:
    rounded: "{rounded.theme-key}"
    width: "28px"
    height: "29px"
  palette:
    rounded: "{rounded.control}"
    typography: "{typography.body}"
---

# Design System: ACTION75

## Overview

**Creative North Star: "外壳刻线"**

A continuous surface carries the contours of the ACTION75 case across the navigation, wallpaper wall, and inspector. Paper and Graphite share the same geometry and use a neutral palette so the wallpaper artwork remains the focus.

Open-line icons and a horizontal wash mark the active page. The wall uses real wallpaper artwork at the display’s 8:3 ratio, and the right inspector keeps live controls within reach.

- One case surface across the full workspace.
- Real wallpaper artwork in its original proportions.
- An adjustable inspector with separate foreground and background controls.

## Colors

Paper (纸白) is the default. Graphite (石墨灰) reverses the contrast with light ink over a dark ground. Each theme has a highlight, ground, and shade for the case surface, with separate ink for text and open-line icons. Use muted ink for secondary labels, the control face and edge for editable surfaces, and selection tones for native control states. Artwork and palette swatches carry pack colors. Error text uses the theme’s dedicated red.

## Typography

Use the embedded Action75 Display font for page and inspector headings. Its SIL Open Font License 1.1 is in `Licenses/Action75Display-OFL.txt`. Microsoft YaHei supplies body text and display fallback glyphs when available, followed by egui’s proportional fallback. The tracked ACTION75 label uses the body family. Use smaller labels for parameter values and status.

## Layout

All lengths in the tokens represent egui logical units. The default window is 1380×880, with a 1080×680 minimum. The chrome has a 50-unit top bar and a 122-unit navigation rail. Below 760 units of viewport height, vertical spacing tightens and the library heading uses 33 units.

The wallpaper wall uses three columns, dropping to two below 536 units of available gallery width. The gap narrows below 650 units. Gallery drawing is limited to visible rows. The right inspector defaults to 262 units, with a 224–400 range and a window-dependent cap that reserves room for the wall. Gallery and inspector scroll independently. The inspector remembers its open state and width.

## Elevation & Depth

A single mesh carries the highlight, ground and lower-right shade across the full viewport. Low-opacity case contours follow continuous paths behind all regions. A fine grip marks the inspector edge. Native popups and modals keep their egui surfaces. The two theme keys use a small offset base for press feedback.

## Shapes

Keep contours long and low-contrast with curved changes of direction and short ticks. Navigation icons are open strokes with rounded joins. Controls have small corners. Wallpaper images use a three-unit corner radius and a separate focus outline. Inspector sliders have a two-unit rail and a compact rounded rectangular handle.

## Components

- **Navigation and theme.** Center each open-line icon above its label. The selected row has a horizontal wash with a rounded left edge that fades into the shared surface. Show a focus outline for keyboard navigation. Connection status and refresh sit at the bottom of the rail. Paper and Graphite keycaps sit at the top right.
- **Search.** Place the open magnifying glass and `/` shortcut beside the single-line field. Either pressing `/` outside text input or clicking the shortcut focuses the field and reveals its underline.
- **Wallpaper wall.** Draw cached wallpaper artwork at 8:3, with captions below the images and a dot beside the current wallpaper. Hover adds a subtle wash without moving the artwork. A single click activates a wallpaper when the player is connected.
- **Inspector.** Use the current wallpaper name above the controls and keep playback in the footer. Resize from the left grip and reopen with the chevron near search. Show basic foreground and background controls with independent advanced disclosures wherever the pack defines them. Sliders place labels and current values above the track.
- **Palette.** Show names and swatches in the preset dropdown. Clicking a preset swatch opens its color picker and converts the palette to custom. Swatches wrap as the inspector narrows. Add and remove controls appear in custom mode.
- **Motion.** Hover and active markers use 130 ms, page changes and the inspector title and controls use 170 ms, theme changes use 200 ms, and the inspector reveal uses 220 ms. Ease out and settle. When Windows Animation effects is off, apply changes immediately.

## Do's and Don'ts

- **Do** preserve preview proportions and use the caption dot to mark the current wallpaper.
- **Do** keep the case traces continuous and leave controls and previews legible.
- **Do** keep parameter values visible and foreground and background controls independent.
- **Do** keep labels functional and respect Windows Animation effects.
