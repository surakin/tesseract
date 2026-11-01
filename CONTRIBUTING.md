# Contributing to Tesseract

Contributions are welcome. The one thing to keep in mind is **UI parity**: Tesseract should look and behave the same on every platform. If you only care about one platform, forking is fine too.

- **Report a bug or suggest a feature:** open a GitHub issue, or ask in [#tesseract-client:matrix.org](https://matrix.to/#/#tesseract-client:matrix.org).
- **Try it out:** the [user guide](docs/USER-GUIDE.md) explains how the app works.
- **Build it:** [docs/BUILD.md](docs/BUILD.md).
- **Change the code:** [docs/DEVELOPING.md](docs/DEVELOPING.md) covers the rules and the common recipes (new string, setting, shortcut, Rust↔C++ call, test). [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) is the big picture and [STYLE.md](STYLE.md) is the code style.

AI-assisted contributions are fine as long as a human has reviewed and tested the code (see [DEVELOPING.md](docs/DEVELOPING.md#ai-assisted-development)).

Before you open a pull request, build and run the tests on your platform, add a line to the Unreleased section of [CHANGES.md](CHANGES.md), and say which platforms you could not test.
