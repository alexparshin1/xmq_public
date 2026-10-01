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
want. The default URL is `https://localhost:18883`, matching the sample
configuration. Edge handles certificate warnings in its normal way.

## Linux

The Linux version uses GLib/GIO for a StatusNotifier tray icon and launches a
Chromium-based browser in app mode. It does not require Qt. Starting the tray
does not open a browser window. Clicking the tray icon opens the console; later
clicks hide or show that same window. The tray menu has **Open XMQ Console**
and **Quit** actions. Closing the browser window leaves the tray application
running. Window toggling requires an X11 or XWayland session. Chromium uses a
separate profile under the user's cache directory for this window.

On Debian or Ubuntu, install the small development package and a browser:

```sh
sudo apt install libglib2.0-dev libx11-dev chromium
```

On FreeBSD, the same source should build with GLib/GIO, X11, CMake,
pkg-config, and Chromium installed. It has not been tested on FreeBSD.

Build and install independently of the broker:

```sh
cmake -S tray -B tray/build -DCMAKE_BUILD_TYPE=Release
cmake --build tray/build
cmake --install tray/build --prefix "$HOME/.local"
```

On Linux, build the package for the current distribution with the `package`
target after configuring and building:

```sh
cmake --build tray/build --target package
```

The target creates a `.deb` on Debian-based systems or an `.rpm` on RPM-based
systems in `tray/build`. It installs the executable, desktop launcher, and icon
under `/usr/local`. The Debian build needs `dpkg-deb`; the RPM build needs `rpmbuild`.

Run `xmq_tray` from an application menu or shell. The default URL is
`https://localhost:18883`, matching the repository's sample broker
configuration. For a different web console port, run:

```sh
xmq_tray --url https://localhost:PORT
```

Use `--browser /path/to/browser` to select a different Chromium-based browser.
For an installed desktop launcher, change its `Exec` line if your console uses
a different URL. The browser handles a self-signed certificate warning.

A desktop with a StatusNotifier/AppIndicator tray host is required. KDE Plasma
and many other desktops provide one; GNOME Shell may need an AppIndicator
extension. The executable reports a clear error when no tray host is available.

For MATE, add **Notification Area** to the panel and enable its StatusNotifier
support:

```sh
gsettings set org.mate.panel enable-sni-support true
```

Restart the MATE panel or log out and in, then verify that
`busctl --user list | grep org.kde.StatusNotifierWatcher` shows an active owner.
The separately listed `org.x.StatusNotifierWatcher` is an activatable xapp
service and does not itself show that MATE's SNI host is running.
