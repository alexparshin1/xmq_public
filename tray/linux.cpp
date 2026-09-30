#include <gio/gio.h>

#include <array>
#include <string>

namespace {
constexpr const char* itemPath = "/StatusNotifierItem";
constexpr const char* menuPath = "/Menu";
constexpr const char* itemInterface = "org.kde.StatusNotifierItem";
constexpr const char* menuInterface = "com.canonical.dbusmenu";

const char* xml = R"xml(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method>
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
  </interface>
  <interface name="com.canonical.dbusmenu">
    <method name="GetLayout">
      <arg type="i" direction="in"/><arg type="i" direction="in"/>
      <arg type="as" direction="in"/><arg type="u" direction="out"/>
      <arg type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="Event">
      <arg type="i" direction="in"/><arg type="s" direction="in"/>
      <arg type="v" direction="in"/><arg type="u" direction="in"/>
    </method>
    <method name="AboutToShow"><arg type="i" direction="in"/><arg type="b" direction="out"/></method>
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
  </interface>
</node>)xml";

struct App {
    GMainLoop* loop = nullptr;
    std::string url = "https://localhost:1883";
    std::string browser;
};

bool validUrl(const std::string& url) {
    GError* error = nullptr;
    GUri* uri = g_uri_parse(url.c_str(), G_URI_FLAGS_NONE, &error);
    if (!uri) {
        g_clear_error(&error);
        return false;
    }
    const char* host = g_uri_get_host(uri);
    const bool valid = g_strcmp0(g_uri_get_scheme(uri), "https") == 0 && host &&
        (g_strcmp0(host, "localhost") == 0 || g_strcmp0(host, "127.0.0.1") == 0 ||
         g_strcmp0(host, "::1") == 0) && g_uri_get_port(uri) > 0 &&
        g_uri_get_userinfo(uri) == nullptr && g_uri_get_query(uri) == nullptr &&
        g_uri_get_fragment(uri) == nullptr &&
        (!g_uri_get_path(uri) || !*g_uri_get_path(uri) || g_strcmp0(g_uri_get_path(uri), "/") == 0);
    g_uri_unref(uri);
    return valid;
}

void openConsole(App* app) {
    const std::string arg = "--app=" + app->url;
    gchar* argv[] = {const_cast<gchar*>(app->browser.c_str()), const_cast<gchar*>(arg.c_str()), nullptr};
    GError* error = nullptr;
    if (!g_spawn_async(nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &error)) {
        g_printerr("Cannot launch browser: %s\n", error->message);
        g_clear_error(&error);
    }
}

GVariant* iconPixmap() {
    // StatusNotifier pixmaps contain ARGB bytes in network order.
    std::array<guint8, 32 * 32 * 4> pixels{};
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const bool mark = (x >= 5 && x < 27 && y >= 8 && y < 24 &&
                               (abs(x - y) < 3 || abs((31 - x) - y) < 3));
            const size_t i = static_cast<size_t>(y * 32 + x) * 4;
            pixels[i] = mark ? 255 : 0;
            pixels[i + 1] = mark ? 81 : 0;
            pixels[i + 2] = mark ? 215 : 0;
            pixels[i + 3] = mark ? 177 : 0;
        }
    }
    GVariantBuilder pixmaps;
    g_variant_builder_init(&pixmaps, G_VARIANT_TYPE("a(iiay)"));
    g_variant_builder_add_value(&pixmaps, g_variant_new("(ii@ay)", 32, 32,
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, pixels.data(), pixels.size(), sizeof(guint8))));
    return g_variant_builder_end(&pixmaps);
}

GVariant* itemProperty(const char* name) {
    if (g_str_equal(name, "Category")) return g_variant_new_string("ApplicationStatus");
    if (g_str_equal(name, "Id")) return g_variant_new_string("xmq-tray");
    if (g_str_equal(name, "Title")) return g_variant_new_string("XMQ Console");
    if (g_str_equal(name, "Status")) return g_variant_new_string("Active");
    if (g_str_equal(name, "IconName")) return g_variant_new_string("xmq-tray");
    if (g_str_equal(name, "IconPixmap")) return iconPixmap();
    if (g_str_equal(name, "Menu")) return g_variant_new_object_path(menuPath);
    if (g_str_equal(name, "ItemIsMenu")) return g_variant_new_boolean(FALSE);
    return nullptr;
}

GVariant* menuProperty(const char* name) {
    if (g_str_equal(name, "Version")) return g_variant_new_uint32(3);
    if (g_str_equal(name, "TextDirection")) return g_variant_new_string("ltr");
    if (g_str_equal(name, "Status")) return g_variant_new_string("normal");
    return nullptr;
}

GVariant* layout() {
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    for (const auto& entry : {std::pair<int, const char*>{1, "Open XMQ Console"}, {2, "Quit"}}) {
        GVariantBuilder childProps;
        g_variant_builder_init(&childProps, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&childProps, "{sv}", "label", g_variant_new_string(entry.second));
        GVariantBuilder noChildren;
        g_variant_builder_init(&noChildren, G_VARIANT_TYPE("av"));
        g_variant_builder_add_value(&children, g_variant_new_variant(
            g_variant_new("(i@a{sv}@av)", entry.first,
                          g_variant_builder_end(&childProps), g_variant_builder_end(&noChildren))));
    }
    return g_variant_new("(i@a{sv}@av)", 0, g_variant_builder_end(&props),
                         g_variant_builder_end(&children));
}

void methodCall(GDBusConnection*, const char*, const char*, const char* interface,
                const char* method, GVariant* parameters, GDBusMethodInvocation* invocation,
                gpointer data) {
    auto* app = static_cast<App*>(data);
    if (g_str_equal(interface, itemInterface)) {
        if (g_str_equal(method, "Activate") || g_str_equal(method, "SecondaryActivate"))
            openConsole(app);
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (g_str_equal(method, "GetLayout")) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@(ia{sv}av))", 1, layout()));
    } else if (g_str_equal(method, "AboutToShow")) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
    } else if (g_str_equal(method, "Event")) {
        gint32 id = 0;
        const char* event = nullptr;
        GVariant* unused = nullptr;
        guint32 timestamp = 0;
        g_variant_get(parameters, "(isvu)", &id, &event, &unused, &timestamp);
        g_variant_unref(unused);
        if (g_str_equal(event, "clicked")) {
            if (id == 1) openConsole(app);
            if (id == 2) g_main_loop_quit(app->loop);
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }
}

GVariant* getProperty(GDBusConnection*, const char*, const char*, const char* interface,
                      const char* name, GError**, gpointer) {
    return g_str_equal(interface, itemInterface) ? itemProperty(name) : menuProperty(name);
}

const GDBusInterfaceVTable vtable = {methodCall, getProperty, nullptr, {nullptr}};
} // namespace

int main(int argc, char** argv) {
    App app;
    for (int i = 1; i < argc; ++i) {
        if (g_str_equal(argv[i], "--url") && i + 1 < argc) app.url = argv[++i];
        else if (g_str_equal(argv[i], "--browser") && i + 1 < argc) app.browser = argv[++i];
        else {
            g_printerr("Usage: xmq_tray [--url https://localhost:PORT] [--browser chromium]\n");
            return 2;
        }
    }
    if (!validUrl(app.url)) {
        g_printerr("The console URL must use HTTPS on localhost or a loopback address, with a port.\n");
        return 2;
    }
    if (app.browser.empty()) {
        for (const char* candidate : {"chromium", "chromium-browser", "google-chrome", "microsoft-edge"}) {
            gchar* path = g_find_program_in_path(candidate);
            if (path) { app.browser = path; g_free(path); break; }
        }
    }
    if (app.browser.empty()) {
        g_printerr("No Chromium-based browser found; use --browser to specify one.\n");
        return 1;
    }
    GError* error = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!bus) {
        g_printerr("No desktop session bus: %s\n", error->message);
        g_clear_error(&error);
        return 1;
    }
    GDBusNodeInfo* info = g_dbus_node_info_new_for_xml(xml, &error);
    if (!info) {
        g_printerr("Invalid D-Bus interface: %s\n", error->message);
        g_clear_error(&error);
        g_object_unref(bus);
        return 1;
    }
    const guint itemRegistration = g_dbus_connection_register_object(
        bus, itemPath, info->interfaces[0], &vtable, &app, nullptr, &error);
    const guint menuRegistration = g_dbus_connection_register_object(
        bus, menuPath, info->interfaces[1], &vtable, &app, nullptr, &error);
    if (!itemRegistration || !menuRegistration) {
        g_printerr("Could not register tray interfaces: %s\n", error ? error->message : "unknown error");
        g_clear_error(&error);
        g_dbus_node_info_unref(info);
        g_object_unref(bus);
        return 1;
    }
    GVariant* result = g_dbus_connection_call_sync(
        bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
        g_variant_new("(s)", g_dbus_connection_get_unique_name(bus)), nullptr,
        G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &error);
    if (!result) {
        g_printerr("No StatusNotifier tray host: %s\n", error->message);
        g_clear_error(&error);
        g_dbus_node_info_unref(info);
        g_object_unref(bus);
        return 1;
    }
    g_variant_unref(result);
    app.loop = g_main_loop_new(nullptr, FALSE);
    openConsole(&app);
    g_main_loop_run(app.loop);
    g_main_loop_unref(app.loop);
    g_dbus_connection_unregister_object(bus, menuRegistration);
    g_dbus_connection_unregister_object(bus, itemRegistration);
    g_dbus_node_info_unref(info);
    g_object_unref(bus);
    return 0;
}
