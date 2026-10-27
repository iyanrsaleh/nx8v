NX8V 0.2.2

Terminal text is English.

- `nx8v --help` and the other commands print English
- HTTP/2 and TLS from 0.2.0 stay in this build

| File | Contents |
|---|---|
| `nx8v-win-x64.zip` | `nx8v.exe` and `WebView2Loader.dll` |
| `nx8v-linux-x64.tar.gz` | Linux x64 binary `nx8v` (glibc 2.34, OpenSSL 3) |
| `install.ps1` | install the Windows release into `%LOCALAPPDATA%\nx8v\bin` |
| `install.sh` | install the Linux release into `~/.local/bin` |

Windows:

```powershell
powershell -ExecutionPolicy Bypass -Command "irm https://github.com/iyanrsaleh/nx8v/releases/latest/download/install.ps1 | iex"
```

Linux:

```sh
curl -fsSL https://github.com/iyanrsaleh/nx8v/releases/latest/download/install.sh | sh
```
