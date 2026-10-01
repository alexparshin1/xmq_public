#include <windows.h>
#include <shellapi.h>

#include <initializer_list>
#include <string>

namespace {
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kOpen = 1001;
constexpr UINT kQuit = 1002;
constexpr UINT kTrayId = 1;
std::wstring consoleUrl = L"https://localhost:18883";
UINT taskbarCreated = 0;

bool validUrl(const std::wstring& url) {
    // Accept a local HTTPS endpoint and a numeric port only. This also keeps
    // the Edge command line argument free of quoting and host-name tricks.
    size_t portStart = std::wstring::npos;
    for (const auto* prefix : {L"https://localhost:", L"https://127.0.0.1:", L"https://[::1]:"}) {
        const std::wstring localPrefix(prefix);
        if (url.rfind(localPrefix, 0) == 0) {
            portStart = localPrefix.size();
            break;
        }
    }
    if (portStart == std::wstring::npos)
        return false;
    const size_t portEnd = url.find(L'/', portStart);
    const auto port = url.substr(portStart, portEnd - portStart);
    if (port.empty() || port.size() > 5 ||
        port.find_first_not_of(L"0123456789") != std::wstring::npos)
        return false;
    const unsigned long number = std::stoul(port);
    return number > 0 && number <= 65535 &&
           (portEnd == std::wstring::npos || portEnd == url.size() - 1);
}

void addTrayIcon(HWND window) {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window;
    icon.uID = kTrayId;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!icon.hIcon)
        icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpyW(icon.szTip, L"XMQ Console");
    Shell_NotifyIconW(NIM_ADD, &icon);
    icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon);
}

void openConsole(HWND window) {
    const std::wstring parameters = L"--app=\"" + consoleUrl + L"\"";
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(window, L"open", L"msedge.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL));
    if (result <= 32)
        MessageBoxW(window, L"Could not start Microsoft Edge. Install Edge or check its App Paths registration.",
                    L"XMQ Console", MB_OK | MB_ICONERROR);
}

void showMenu(HWND window) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kOpen, L"Open XMQ Console");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kQuit, L"Quit");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == taskbarCreated) {
        addTrayIcon(window);
        return 0;
    }
    switch (message) {
    case kTrayMessage:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            openConsole(window);
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            showMenu(window);
            break;
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == kOpen)
            openConsole(window);
        else if (LOWORD(wParam) == kQuit)
            DestroyWindow(window);
        return 0;
    case WM_DESTROY: {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window;
        icon.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &icon);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    if (*commandLine) {
        std::wstring args = commandLine;
        const std::wstring prefix = L"--url ";
        if (args.rfind(prefix, 0) == 0) {
            consoleUrl = args.substr(prefix.size());
            if (consoleUrl.size() >= 2 && consoleUrl.front() == L'"' && consoleUrl.back() == L'"')
                consoleUrl = consoleUrl.substr(1, consoleUrl.size() - 2);
        } else {
            MessageBoxW(nullptr, L"Usage: xmq_tray.exe [--url https://localhost:PORT]",
                        L"XMQ Console", MB_OK | MB_ICONERROR);
            return 2;
        }
    }
    if (!validUrl(consoleUrl)) {
        MessageBoxW(nullptr, L"The console URL must use HTTPS on localhost or a loopback address.",
                    L"XMQ Console", MB_OK | MB_ICONERROR);
        return 2;
    }

    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"XmqTrayWindow";
    if (!RegisterClassW(&windowClass))
        return 1;
    // A hidden top-level window receives the TaskbarCreated broadcast after Explorer restarts.
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"XMQ Console", 0,
                                  0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window)
        return 1;
    addTrayIcon(window);
    openConsole(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
