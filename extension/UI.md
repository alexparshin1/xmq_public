# Declarative extension forms — version 1

An extension author needs the installed public ABI headers and CMake helper, not the XMQ source
tree. Settings, their types, defaults and server-side validation remain in the library. An optional
**`<library filename>.ui.json`** beside it adds presentation, without JavaScript or changes to XMQ.
For example, `libxmq_ldap.so` is accompanied by `libxmq_ldap.so.ui.json`; on Windows the filename
ends in `.dll.ui.json`. The broker sends this data through its authenticated extension-control API;
the browser does not fetch filesystem paths or load extension scripts.

```json
{
  "version": 1,
  "sections": [
    {
      "title": "Connection",
      "description": "Choose the directory profile, then check the connection settings.",
      "fields": [
        {"name": "directory_type"},
        {"name": "host"},
        {
          "name": "account_state_attribute",
          "visible_when": {"setting": "account_state_check", "values": ["yes", "auto"]}
        },
        {"name": "group_mappings", "widget": "multiline"}
      ]
    }
  ],
  "presets": [
    {
      "name": "ad",
      "label": "Active Directory",
      "description": "Directory profile only; connection settings are left alone.",
      "choice_setting": "directory_type",
      "choice_value": "ad",
      "values": {"directory_type": "ad", "account_state_check": "yes"}
    }
  ]
}
```

## Contract

- `version` must be `1`; `sections` is an array, in display order. Each section has a nonempty
  `title`, an optional plain-text `description`, and a `fields` array.
- Each field references a setting by `name`. A setting may be listed only once. Unlisted settings
  still appear after the sections, including configured but undeclared keys: a layout must not hide
  a misspelling or make a newly introduced setting inaccessible.
- `visible_when` and `enabled_when` are optional conditions. Each references one declared,
  non-secret `setting` and lists the string `values` for which it is true. Conditions use current
  form values, including defaults and unapplied edits. No expressions or code are evaluated.
- A hidden or disabled control **retains its value** and is still submitted and validated on Apply.
  Hiding a setting does not disable its server-side behavior. No cascade of value-changing rules
  is run when visibility changes.
- `widget: "multiline"` selects a textarea for a string setting. Other types keep their ABI widgets.
- Optional `presets` replaces the ABI preset list; omit it to use the library's presets. An empty
  array deliberately offers none. Each preset has a nonempty unique `name`, optional `label` and
  `description`, and `values` containing string `setting-name: value` pairs.
- Presets may reference only declared, non-secret settings. They change only listed settings.
  Optional `choice_setting` / `choice_value` binds a preset to a choice option and fills the form
  when that option is selected. The preset must itself set that choice to the bound value.
- Presets and conditions never apply settings. **Apply remains mandatory**, and the broker still
  validates and accepts or refuses the configuration independently of the form.
- The maximum file size is 64 KiB; individual strings are limited to 4096 characters. Unknown
  properties, invalid references, unsupported widgets and unsupported versions invalidate the
  whole description. The GUI shows a warning and the ordinary ABI-generated form instead. A missing
  file uses the ordinary form without a warning. No HTML, scripts or external assets are supported.

The data is part of the installed extension, not operator configuration. Do not put credentials in
it. Updating the sidecar is reflected by refreshing the Extensions page; no broker rebuild is needed.
As with changing an ABI declaration, distribute the library and sidecar as one versioned package.

## Build and installation

The SDK installs `XmqExtension.cmake` under `share/xmq/cmake`, and headers under
`include/xmq/extension`. Include the helper and set `XMQ_EXTENSION_INCLUDE_DIR` to `include/xmq`:

```cmake
XMQ_ADD_EXTENSION(xmq_example SOURCES Example.cpp UI "${CMAKE_CURRENT_SOURCE_DIR}/example.ui.json")
INSTALL(TARGETS xmq_example LIBRARY DESTINATION lib/xmq-extensions
        RUNTIME DESTINATION lib/xmq-extensions)
```

`UI` copies the file beside the built library and installs it under `lib/xmq-extensions` with the
platform-correct name. Installing the library remains the extension project's responsibility.
Without `UI`, the helper behaves as before. The ABI is unchanged.

`ldap/` is a complete example: copy that directory outside the XMQ tree and configure it against an
installed SDK (and the OpenLDAP/OpenSSL development packages):

```sh
cmake -S ldap -B ldap-build -DCMAKE_PREFIX_PATH="$HOME/.local" -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build ldap-build
cmake --install ldap-build
```

The result is a library and a JSON form. Neither compilation nor installation requires access to
the broker or React sources. LDAP keeps its ABI presets too, as a fallback for older interfaces or
an installation without the sidecar; the sidecar adds layout and declarative visibility rules.
