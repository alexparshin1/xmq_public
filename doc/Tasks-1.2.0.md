# XMQ 1.2.0 — tasks

Work planned for 1.2.0, in the order it should be done. An item leaves this file when it is
released, not when it is coded: the changelog records what shipped, this file records what is
still owed.

## Planned

- [ ] **One extension, many settings sets (named ExtensionInstances).** — the headline feature of 1.2.0.
  Today the Extensions screen is a list of extensions, and each extension owns exactly one flat
  settings map: two exports to two databases require loading the same library twice, which
  `ExtensionHost` explicitly refuses. Change that to a list of *settings sets*, each with its own
  label, using one extension library.

  Config entries gain an `extension_instance` label. Several entries may share a `name`/`library` as long as
  their ExtensionInstances differ; the host keys loaded instances by name + ExtensionInstance and exposes it
  to the extension through `hostSetting`. Existing entries without a label keep working as one
  default ExtensionInstance, so no config migration is needed.

  Data integrations configured from the screen are not written to config fragments. They are rows
  in `xmq_data_integration` in the shared cluster SQL database: `node_id` is a foreign key to
  `xmq_node`, `extension` and `extension_instance` are the stable technical identity, `name` is the friendly
  editable name (unique within the node, case-insensitive), `enabled` and `settings_json` hold the
  rest. Renaming a data integration never changes its ExtensionInstance, so its persistent queue identity
  survives. Every SQL database holds a local `xmq_node` row keyed by the node's GUID, including
  SQLite; SQLite refuses a second node's row while PostgreSQL/MySQL may hold many. Brokers with no
  database at all keep file-backed settings sets.

  For the export extensions this must give each settings set its own topic filter/SQL rules, sink,
  SQLite queue, retry worker and dead-letter topic. The Extensions screen shows a flat list of
  **Data Integrations**; **Add Data Integration** opens a dialog with a combo box of the available
  kinds and the selected kind's description, plus an editable name generated as "Message Export",
  "Log Export" and so on and checked for uniqueness. Accepting it creates the row and opens that
  data integration's settings immediately. Acceptance: two `message_export` data integrations
  writing to two different databases with different filters and queue files work at the same time,
  survive a settings change and a broker restart, and the farm is green on every system.

- [ ] **Rename `ControlService` to `RestService`.**
  Found in the 1.2.0 farm run `2026-11-23-2112`: the C++ class in
  `server/ControlService.{h,cpp}` collides with the Win32 `ControlService()` function from
  `winsvc.h`. With `using namespace xmq;`, an unqualified `ControlService controlService(...)`
  is ambiguous on Windows (`error C2872: 'ControlService': ambiguous symbol`) in
  `test/PersistenceTests/ReliableMessageStoreTests.cpp`. Qualifying that one use as
  `xmq::ControlService` is a workaround, not a fix: any other translation unit that includes
  Windows headers and uses the type the same way breaks again.

  Rename the class and its files to `RestService`, and the listener that owns it
  (`server/ControlServiceListener.{h,cpp}`) to `RestServiceListener`, so no type in the tree
  keeps the colliding name. The same applies to the aliases and test fixtures that carry it
  (`SControlServiceListener`, `test/ControlServiceTests/`, `XMQ_ControlServiceTests`) and to the
  references in `server/`, `test/`, CMake source lists and documentation that names the type.
  Revert the `xmq::` qualification workaround once the rename is in.

  The wire protocol does not change: HTTP paths, JSON fields, service operation names,
  configuration keys and the web interface stay exactly as they are. This is a source-level
  rename only. Acceptance: the full Windows solution builds without `C2872`, and the
  HTTP/control test suite remains unchanged and green on every farm system.
