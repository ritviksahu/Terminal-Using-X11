## System Requirements

- **Libraries:** X11 (via XQuartz)
- **Compiler:** Apple Clang / g++ (C++17)
- **POSIX APIs:** `fork`, `execl`, `pipe`, `dup2`, `signal`

---

## Install Dependencies

### 1. Install XQuartz (X11 for macOS)
```bash
brew install --cask xquartz
```
Log out and back in to activate X server.

### 2. Install Command Line Tools (if not installed)
```bash
xcode-select --install
```

### 3. Verify X11
```bash
echo $DISPLAY
# should show something like /private/tmp/com.apple.launchd.xxx/org.xquartz:0
```
If not:
```bash
export DISPLAY=:0
```

---

## Build

> Use Homebrew g++
```bash
brew install gcc
g++ myTerm_25CS60R19.cpp  -o myTerm -std=c++17 -I/opt/X11/include -L/opt/X11/lib -lX11 -pthread
```

---

## Run

```bash
./myTerm
```
If the window doesn't appear:
```bash
export DISPLAY=:0
```

