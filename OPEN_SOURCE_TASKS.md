# Open source follow-up tasks

- [ ] Make `docker/build.sh` work from a standalone XMQ checkout. `docker/versions.sh`
  currently reads SPTK's version from a sibling repository. Accept an explicit
  version or read a version recorded in this repository; verify the documented
  `./build.sh` command from a fresh clone.
- [x] Add a public clean-checkout build and a service-free GoogleTest subset,
  without private extensions. Document the services used by the larger suite.
- [x] Add `SECURITY.md` with supported versions, a private vulnerability-reporting
  route, and the expected response process. Private reporting is enabled on GitHub.
- [ ] Account for vendored third-party licenses in source and release artifacts.
  Include the Apache 2.0 license and applicable attribution for `gtl/`, and audit
  the other bundled dependencies and installers.
- [ ] Add contributor guidance. Document issue and pull request expectations,
  build and test conventions, and contribution licensing in `CONTRIBUTING.md`;
  adopt and link a code of conduct.
