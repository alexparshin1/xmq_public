# XMQ tray console

This standalone application keeps an XMQ icon in the desktop tray. The tray menu
opens the console and exits the application.

## Windows

The Windows version uses the native Windows tray API and launches Microsoft
Edge in app mode, giving the console its own window without Edge tabs or
toolbar. It does not require Qt. Edge must be installed.

Build with CMake and Visual Studio (or another Windows C++ toolchain):

```powershell
cmake -S tray -B tray/build
cmake --build tray/build --config Release
```

Run `tray/build/Release/xmq_tray.exe`, or copy the executable wherever you
want. Use `--url https://localhost:18883` if your console runs on the sample
configuration's port. Edge handles certificate warnings in its normal way.

## Linux

The Linux version uses GLib/GIO for a StatusNotifier tray icon and launches a
Chromium-based browser in app mode. It does not require Qt. Clicking the tray
icon opens a console window without browser tabs or toolbar. The tray menu has
**Open XMQ Console** and **Quit** actions. Closing the browser window leaves
the tray application running.

On Debian or Ubuntu, install the small development package and a browser:

```sh
sudo apt install libglib2.0-dev chromium
```

Build and install independently of the broker:

```sh
cmake -S tray -B tray/build -DCMAKE_BUILD_TYPE=Release
cmake --build tray/build
cmake --install tray/build --prefix "$HOME/.local"
```

Run `xmq_tray` from an application menu or shell. The default URL is
`https://localhost:1883`, as requested. The repository's sample broker
configuration uses port **18883** for the web console, so with that setup run:

```sh
xmq_tray --url https://localhost:18883
```

Use `--browser /path/to/browser` to select a different Chromium-based browser.
For an installed desktop launcher, change its `Exec` line if your console uses
a different URL. The browser handles a self-signed certificate warning.

A desktop with a StatusNotifier/AppIndicator tray host is required. KDE Plasma
and many other desktops provide one; GNOME Shell may need an AppIndicator
extension. The executable reports a clear error when no tray host is available.
