# XMQ 1.2.0 — tasks

Work planned for 1.2.0, in the order it should be done. An item leaves this file when it is
released, not when it is coded: the changelog records what shipped, this file records what is
still owed.

## Planned

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
