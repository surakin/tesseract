# GNOME Portal Dark Theme Detection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix dark theme detection for the Qt6 shell on GNOME when `QGnomePlatform` is not installed or Qt < 6.6, by falling back to the XDG Desktop Portal `org.freedesktop.appearance color-scheme` key.

**Architecture:** `os_color_scheme_()` (a virtual override in `MainWindow`) already returns the theme from `QStyleHints::colorScheme()`. On GNOME without the platform plugin, this returns `Qt::ColorScheme::Unknown`. We add a cached `portal_color_scheme_` int member (0=no preference, 1=dark, 2=light), populate it at startup via a `QDBusInterface::call("Read", ...)` to `org.freedesktop.portal.Settings`, and re-use the existing `SettingChanged` signal on that interface to keep it current. `os_color_scheme_()` returns `ThemeMode::Dark` when the portal says 1, `ThemeMode::Light` otherwise — but only consulted when Qt itself returns `Unknown`. On KDE, Qt always returns a real value so the portal path is never taken.

**Tech Stack:** `QDBusInterface`, `QDBusConnection::sessionBus()`, `QDBusVariant` (already used in `ui/linux-qt/src/LinuxNotifier.cpp`); `private slots:` (existing `private slots:` section in `MainWindow.h`).

---

## File Inventory

| File | Change |
|------|--------|
| `ui/linux-qt/src/MainWindow.h` | Add `int portal_color_scheme_` member; add `read_portal_color_scheme_()` and `on_portal_setting_changed_()` to `private slots:` |
| `ui/linux-qt/src/MainWindow.cpp` | Implement `read_portal_color_scheme_()`; update `os_color_scheme_()` to fall back; connect `SettingChanged` in constructor |

No new files. No test files — the logic is a single ternary (`val == 1 ? Dark : Light`) and the D-Bus wiring follows the identical pattern used in `LinuxNotifier.cpp`. Manual verification is the appropriate test method here (see Task 3).

---

## Task 1: Add `portal_color_scheme_` member and update `os_color_scheme_()`

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.h` (private members section, near other `int` members)
- Modify: `ui/linux-qt/src/MainWindow.cpp` (lines 3334–3338 — the `os_color_scheme_` body)

- [ ] **Step 1: Read the files**

  Read `ui/linux-qt/src/MainWindow.h` lines 1–60 and around line 190 (where `os_color_scheme_` is declared) to understand the member layout.
  Read `ui/linux-qt/src/MainWindow.cpp` lines 3330–3345 to see the current body.

- [ ] **Step 2: Add `portal_color_scheme_` to the private members in `MainWindow.h`**

  Find the block of `int` / `bool` private members (in the private section after `private slots:` at line 79). Add:

  ```cpp
  // Cached org.freedesktop.appearance color-scheme portal value.
  // -1 = not yet read, 0 = no preference, 1 = dark, 2 = light.
  int portal_color_scheme_ = -1;
  ```

- [ ] **Step 3: Update `os_color_scheme_()` in `MainWindow.cpp`**

  Replace the current body:

  ```cpp
  tk::ThemeMode MainWindow::os_color_scheme_() const
  {
      return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
          ? tk::ThemeMode::Dark : tk::ThemeMode::Light;
  }
  ```

  with:

  ```cpp
  tk::ThemeMode MainWindow::os_color_scheme_() const
  {
      const auto qt_scheme = QGuiApplication::styleHints()->colorScheme();
      if (qt_scheme != Qt::ColorScheme::Unknown)
          return qt_scheme == Qt::ColorScheme::Dark ? tk::ThemeMode::Dark
                                                    : tk::ThemeMode::Light;
      // Qt could not determine the OS color scheme (common on GNOME without
      // QGnomePlatform / Qt < 6.6). Fall back to the XDG portal value.
      return portal_color_scheme_ == 1 ? tk::ThemeMode::Dark : tk::ThemeMode::Light;
  }
  ```

- [ ] **Step 4: Build to verify the change compiles**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_qt6 2>&1 | grep -E "error:" | head -20
  ```
  Expected: no errors.

- [ ] **Step 5: Commit**

  ```bash
  git add ui/linux-qt/src/MainWindow.h ui/linux-qt/src/MainWindow.cpp
  git commit -m "fix(qt6): fall back to XDG portal color-scheme when Qt returns Unknown"
  ```

---

## Task 2: Implement `read_portal_color_scheme_()` and call it in the constructor

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.h` — add `void read_portal_color_scheme_()` declaration
- Modify: `ui/linux-qt/src/MainWindow.cpp` — implement it; call it in the constructor

- [ ] **Step 1: Declare `read_portal_color_scheme_()` in `MainWindow.h`**

  In the `private:` methods section (not `private slots:`), add:

  ```cpp
  void read_portal_color_scheme_();
  ```

- [ ] **Step 2: Implement `read_portal_color_scheme_()` in `MainWindow.cpp`**

  Add near the bottom of the file, alongside the other private helper implementations:

  ```cpp
  void MainWindow::read_portal_color_scheme_()
  {
      QDBusInterface iface(
          QStringLiteral("org.freedesktop.portal.Desktop"),
          QStringLiteral("/org/freedesktop/portal/desktop"),
          QStringLiteral("org.freedesktop.portal.Settings"),
          QDBusConnection::sessionBus());
      if (!iface.isValid()) return;

      QDBusReply<QDBusVariant> reply = iface.call(
          QStringLiteral("Read"),
          QStringLiteral("org.freedesktop.appearance"),
          QStringLiteral("color-scheme"));
      if (reply.isValid())
          portal_color_scheme_ = reply.value().variant().toInt();
  }
  ```

  Note: `QDBusInterface` and `QDBusVariant` are already included via the Qt D-Bus module. Verify the relevant `#include <QDBusInterface>` / `#include <QDBusVariant>` / `#include <QDBusReply>` are present at the top of `MainWindow.cpp` — if not, add them alongside the existing D-Bus includes (search for `QDBus` in the file to find the right location).

- [ ] **Step 3: Call `read_portal_color_scheme_()` in the constructor**

  In `MainWindow::MainWindow(...)`, find the `apply_current_theme_()` call at line 961 and the `colorSchemeChanged` connection immediately after (lines 964–971). Insert the portal read **before** `apply_current_theme_()` so the cached value is ready when the first theme application runs:

  ```cpp
  read_portal_color_scheme_();   // populate portal fallback before first theme apply
  apply_current_theme_();
  connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
          this, [this] {
              if (tesseract::Settings::instance().theme_pref ==
                  tesseract::Settings::ThemePreference::System)
              {
                  apply_current_theme_();
              }
          });
  ```

- [ ] **Step 4: Build to verify**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_qt6 2>&1 | grep -E "error:" | head -20
  ```
  Expected: no errors.

- [ ] **Step 5: Commit**

  ```bash
  git add ui/linux-qt/src/MainWindow.h ui/linux-qt/src/MainWindow.cpp
  git commit -m "fix(qt6): query XDG portal color-scheme at startup for GNOME fallback"
  ```

---

## Task 3: Subscribe to `SettingChanged` for live updates

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.h` — add `on_portal_setting_changed_` to `private slots:`
- Modify: `ui/linux-qt/src/MainWindow.cpp` — implement slot; connect in constructor

- [ ] **Step 1: Add the slot declaration to `MainWindow.h`**

  In the `private slots:` section (line 79 area), add:

  ```cpp
  void on_portal_setting_changed_(const QString& ns,
                                   const QString& key,
                                   const QDBusVariant& value);
  ```

- [ ] **Step 2: Implement the slot in `MainWindow.cpp`**

  Add alongside `read_portal_color_scheme_()`:

  ```cpp
  void MainWindow::on_portal_setting_changed_(const QString& ns,
                                               const QString& key,
                                               const QDBusVariant& value)
  {
      if (ns != QLatin1String("org.freedesktop.appearance") ||
          key != QLatin1String("color-scheme"))
          return;
      portal_color_scheme_ = value.variant().toInt();
      if (tesseract::Settings::instance().theme_pref ==
          tesseract::Settings::ThemePreference::System)
      {
          apply_current_theme_();
      }
  }
  ```

- [ ] **Step 3: Connect the `SettingChanged` signal in the constructor**

  Immediately after the `read_portal_color_scheme_()` call added in Task 2 (and before `apply_current_theme_()`), add the D-Bus signal connection:

  ```cpp
  read_portal_color_scheme_();
  QDBusConnection::sessionBus().connect(
      QStringLiteral("org.freedesktop.portal.Desktop"),
      QStringLiteral("/org/freedesktop/portal/desktop"),
      QStringLiteral("org.freedesktop.portal.Settings"),
      QStringLiteral("SettingChanged"),
      this,
      SLOT(on_portal_setting_changed_(QString, QString, QDBusVariant)));
  apply_current_theme_();
  connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, ...);
  ```

  The `SLOT()` macro form is required here because `QDBusConnection::connect()` does not support the new-style functor overload with D-Bus signals.

- [ ] **Step 4: Build**

  ```bash
  cmake --build build/linux-qt6-debug --target tesseract_qt6 2>&1 | grep -E "error:" | head -20
  ```
  Expected: no errors.

- [ ] **Step 5: Run the full test suite to confirm no regressions**

  ```bash
  ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -10
  ```
  Expected: all tests pass.

- [ ] **Step 6: Commit**

  ```bash
  git add ui/linux-qt/src/MainWindow.h ui/linux-qt/src/MainWindow.cpp
  git commit -m "fix(qt6): subscribe to XDG portal SettingChanged for live GNOME dark-mode updates"
  ```

---

## Task 4: Update docs

**Files:**
- Modify: `CHANGES.md` — add bullet under today's date
- Modify: `STATUS.md` — remove the GNOME caveat from the theme section if present; bump date and blurb

- [ ] **Step 1: Add entry to `CHANGES.md`**

  Under the current date heading (create one if needed), add:

  ```
  - fix(qt6): dark theme detection on GNOME — Qt6 shell now queries
    `org.freedesktop.portal.Settings` at startup and subscribes to
    `SettingChanged`; `os_color_scheme_()` falls back to the portal value
    when `QStyleHints::colorScheme()` returns `Unknown` (common on GNOME
    without QGnomePlatform or Qt < 6.6)
  ```

- [ ] **Step 2: Commit**

  ```bash
  git add CHANGES.md STATUS.md
  git commit -m "docs: record GNOME portal dark-theme detection fix"
  ```

---

## Manual Verification

Run the app on a GNOME session **without** `qt6ct` / `QGnomePlatform` installed:

```bash
QT_QPA_PLATFORMTHEME= ./build/linux-qt6-debug/ui/linux-qt/tesseract
```

(`QT_QPA_PLATFORMTHEME=` forces Qt to skip the platform theme plugin, reproducing the bare-GNOME condition.)

1. Set GNOME appearance to **Dark** (`gsettings set org.gnome.desktop.interface color-scheme prefer-dark`) → app should switch to dark theme.
2. Set to **Light** (`gsettings set org.gnome.desktop.interface color-scheme default`) → app should switch to light theme.
3. Toggle while the app is running → live update via `SettingChanged` signal.
4. Set Tesseract theme preference to **Light** or **Dark** (in Settings) → portal fallback is bypassed; theme stays fixed regardless of GNOME setting.
5. Run on **KDE** → `QPalette::ColorScheme` returns a real value; portal path never taken; behavior unchanged.
