/**
 * The screen's colours, read from the Borland palette the Turbo Vision apps of 1990 used.
 *
 * Not a byte-for-byte port of Turbo Vision's `cpAppColor` table - that table is a set of BIOS
 * attribute bytes whose meaning depends on a 16-colour DOS palette. What is reproduced here is what
 * those bytes looked like on an EGA screen: a blue desktop with light grey text, grey dialogs with
 * black text, black-on-green buttons, black-on-cyan selection, and shortcut letters that stand out
 * from the text around them.
 *
 * The point of keeping it in one file is that it is a decision about looks, not about behaviour: a
 * screen that wants different colours, or a terminal where blue-on-blue is unreadable, changes this
 * file and nothing else.
 */
#pragma once

namespace theme {

// Terminal control
inline constexpr const char* reset = "\033[0m";

// The blue desktop every Turbo Vision application put its windows on.
inline constexpr const char* desktop = "\033[37;44m";     // light grey on blue
inline constexpr const char* desktopStrong = "\033[1;37;44m";
inline constexpr const char* desktopDim = "\033[34;44m";  // shadow: darker than the desktop

// A window frame and its title, on the desktop.
inline constexpr const char* frame = "\033[37;44m";
inline constexpr const char* frameActive = "\033[1;37;44m";

// A grey dialog: black text on light grey, the way cpGrayDialog reads on screen.
inline constexpr const char* dialog = "\033[30;47m";
inline constexpr const char* dialogFrame = "\033[97;47m"; // white double line on grey
inline constexpr const char* dialogAccent = "\033[92;47m";
inline constexpr const char* windowShadow = "\033[90m";
inline constexpr const char* dialogDim = "\033[37;40m";   // a value that is not set

// What the keyboard is on: Turbo Vision's "selected" style.
inline constexpr const char* selected = "\033[30;46m";    // black on cyan

// A shortcut letter, and a button: black on green for the one that is default.
inline constexpr const char* shortcut = "\033[1;33;44m";  // yellow on blue
inline constexpr const char* button = "\033[30;42m";
inline constexpr const char* buttonKey = "\033[1;30;42m";
inline constexpr const char* buttonActive = "\033[1;37;42m";
inline constexpr const char* buttonHot = "\033[93;42m";
inline constexpr const char* buttonShadowBelow = "\033[90;47m";

// Table and status bars: black on cyan for a column header; the status line at the foot of a
// Turbo Vision screen was a grey bar with black text and the shortcut in red.
inline constexpr const char* header = "\033[30;46m";
inline constexpr const char* status = "\033[30;47m";        // light grey bar
inline constexpr const char* statusKey = "\033[1;31;47m";   // the keys on it, in red
inline constexpr const char* statusDim = "\033[90;47m";     // unavailable actions, grey

// The scrollbar every Turbo Vision window carried down its right edge: cyan, with arrows at the
// ends and a thumb whose length says how much of the list is on screen.
inline constexpr const char* scroll = "\033[30;46m";

// An input field inside a grey dialog was white, not the same grey as the dialog: the two differ
// in the BIOS palette and that difference is what makes a field look editable.
inline constexpr const char* input = "\033[30;107m";        // black on white
inline constexpr const char* inputDim = "\033[37;107m";

// Values that deserve attention without being errors: red on the surface behind them.
inline constexpr const char* warning = "\033[1;31;47m";          // on the dialog grey
inline constexpr const char* warningDesktop = "\033[1;31;44m";   // on the blue desktop

} // namespace theme
