# XMQ tray console (Linux)

This standalone Qt 6 application keeps an XMQ icon in the desktop system tray.
Click it to open or focus the console in its own window, with no browser toolbar
or tabs. Closing the window hides it; **Quit** in the tray menu exits the app.

Requires Qt 6.4 or newer with the Widgets and WebEngineWidgets development
packages.
```sh
sudo apt install qt6-base-dev qt6-webengine-dev
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

For an installed desktop launcher, change its `Exec` line if your console uses
a different URL. The local broker may use a self-signed certificate; the app
asks before accepting an unverified certificate. A desktop environment with a
system tray or StatusNotifier host is required.
