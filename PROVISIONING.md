# Provisioning login defaults

An installer, an admin or a management tool can pre-set two values for the
login screen:

| Value | What it does |
| --- | --- |
| **Homeserver** | Used instead of `matrix.org` when the homeserver field is left empty. It shows as the field's placeholder. |
| **Username** | A localpart (`alice`) or a full Matrix ID (`@alice:example.org`). Prefills the username on the password form, and is sent as the login hint to the server's OAuth / Matrix Authentication Service page. |

Setting either value also hides **Find a server** on the login screen.

These are defaults, not locks. The user can still type a different homeserver,
and a Matrix ID typed into the homeserver field is used as the login hint
instead of the provisioned username. Accounts that are already signed in are
not affected.

The implementation is `client/include/tesseract/provisioning.h`.

## Where the values are read from

Each value is resolved on its own, from the first source that sets it:

1. **Per-user file**, every platform: `provisioning.json` in Tesseract's
   per-user config directory (format [below](#provisioningjson)):

   | Platform | Config directory |
   | --- | --- |
   | Windows | `%APPDATA%\Tesseract\` (e.g. `C:\Users\alice\AppData\Roaming\Tesseract\`) |
   | macOS | `~/Library/Application Support/Tesseract/` |
   | Linux | `$XDG_CONFIG_HOME/tesseract/`, or `~/.config/tesseract/` when `XDG_CONFIG_HOME` is unset |
   | Flatpak | `~/.var/app/io.github.surakin.Tesseract/config/tesseract/` |

   With `--profile=NAME` (see [CLI.md](CLI.md)) the directory name gets the
   profile as a suffix (`Tesseract-work`, `tesseract-work`), so each profile
   can have its own defaults. The platform sources below are shared by all
   profiles.
2. **Platform source**:
   - **Windows**: registry, `HKCU\Software\Tesseract-Matrix` first, then
     `HKLM\Software\Tesseract-Matrix`.
   - **macOS**: the app's preferences, managed (MDM) preferences included.
   - **Linux**: `/etc/tesseract/provisioning.json`.

So a homeserver can come from the system-wide source while the username comes
from the per-user file.

The values are read once, at startup. Restart Tesseract after changing them.

## Value format

- **Homeserver**: a server name (`example.org`, discovered through its
  `.well-known` like a typed one) or a URL (`https://matrix.example.org`).
  Surrounding whitespace and trailing `/` are removed.
- **Username**: a localpart or a full Matrix ID. Surrounding whitespace and a
  leading `@` are removed.
- An empty value counts as not set.

A bare localpart needs a homeserver to become a Matrix ID. With a localpart but
no provisioned homeserver, nothing is prefilled and no hint is sent; use a full
Matrix ID in that case.

## `provisioning.json`

Used by the per-user file on every platform and by the Linux system-wide file.
Both keys are optional:

```json
{
  "default_homeserver": "matrix.example.org",
  "default_username": "alice"
}
```

A file that is not valid JSON is ignored silently, as is a key whose value is
not a string.

## Windows

### Installer switch

The NSIS installer accepts `/DEFAULT_HOMESERVER=<host>`, also together with
`/S`:

```powershell
Tesseract-<version>-AMD64.exe /S /DEFAULT_HOMESERVER=matrix.example.org
```

It writes `HKLM\Software\Tesseract-Matrix\DefaultHomeserver`. The value is
taken unquoted, up to the next space. The installer has no switch for the
username; set it through the registry or the per-user file.

An upgrade (which runs a silent uninstall first) and any `/S` uninstall keep
the value. Only an interactive uninstall removes it.

### Registry

String (`REG_SZ`) values under `Software\Tesseract-Matrix`, in the 64-bit view:

| Value name | Content |
| --- | --- |
| `DefaultHomeserver` | Homeserver |
| `DefaultUsername` | Username |

`HKLM` sets a machine-wide default (for example through Group Policy
Preferences or a deployment script); `HKCU` overrides it per user:

```powershell
New-Item -Path 'HKLM:\Software\Tesseract-Matrix' -Force | Out-Null
Set-ItemProperty -Path 'HKLM:\Software\Tesseract-Matrix' -Name DefaultHomeserver -Value 'matrix.example.org'
Set-ItemProperty -Path 'HKCU:\Software\Tesseract-Matrix' -Name DefaultUsername -Value 'alice'
```

## macOS

Preference keys `DefaultHomeserver` and `DefaultUsername` (strings) in the
domain `im.gnomos.tesseract`.

For a fleet, deliver them in a configuration profile through your MDM, with
`PayloadType` `im.gnomos.tesseract`:

```xml
<dict>
    <key>PayloadType</key>        <string>im.gnomos.tesseract</string>
    <key>PayloadIdentifier</key>  <string>org.example.tesseract.login</string>
    <key>PayloadUUID</key>        <string>REPLACE-WITH-A-UUID</string>
    <key>PayloadVersion</key>     <integer>1</integer>
    <key>DefaultHomeserver</key>  <string>matrix.example.org</string>
    <key>DefaultUsername</key>    <string>alice</string>
</dict>
```

Managed preferences take precedence over the user's own. For a single machine
without MDM:

```bash
defaults write im.gnomos.tesseract DefaultHomeserver matrix.example.org
defaults write im.gnomos.tesseract DefaultUsername alice
```

## Linux

Write `/etc/tesseract/provisioning.json` in the format above, for example from
a configuration-management tool or a site package. It applies to every user on
the machine; a user's own `~/.config/tesseract/provisioning.json` overrides it
value by value.

## Sandboxed builds

The sandboxed packages see different paths. This follows from how the sandboxes
work and has not been tested:

- **Flatpak**: the host's `/etc` is not visible inside the sandbox, so
  `/etc/tesseract/provisioning.json` is not read. Use the per-user file in
  the Flatpak config directory listed above.
- **MSIX**: Windows virtualizes `%APPDATA%` and `HKCU` for packaged apps, so
  values written there from outside the package may not be seen. `HKLM` is the
  safer choice.
