# ProcMonUI — Windows Process Monitor / Task Killer

**Author:** [Bob.Paydar@hp.com](mailto:Bob.Paydar@hp.com)  
**Type:** Windows Desktop App (C++17, Win32 API)

ProcMonUI is a lightweight Windows utility that lets you **view, search, and control processes** from a simple desktop GUI.  
Built in pure Win32 API, it requires no frameworks (MFC/WTL) and is fast, portable, and easy to build.

---

## ✨ Features

- List running processes with:
  - PID
  - Parent PID
  - Memory usage (RSS)
  - Process name
  - Full path
- Search filter (live) by name or path
- Actions:
  - Kill process
  - Suspend process
  - Resume process
  - Apply actions to process tree
- Export process list:
  - JSON (UTF-8, BOM)
  - CSV (UTF-8, BOM)
- Real status bar at the bottom: **"Ready - Bob.Paydar@hp.com"**
- Auto-refresh after actions (refresh, kill, suspend, resume, export)
- No background refresh (does not block user interaction)

---

## 🛠 Build Instructions

Requirements:
- Windows 10 or later
- Visual Studio 2019/2022 (C++ Desktop development tools)

Steps:
1. Create a new **Windows Desktop Wizard → Empty Project** in Visual Studio.
2. Add `ProcMonUI.cpp` (this source file) to **Source Files**.
3. Project Properties:
   - **C/C++ → Language → C++ Language Standard** → `/std:c++17`
   - **Linker → Input → Additional Dependencies** → add:
     ```
     psapi.lib; comctl32.lib; shlwapi.lib;
     ```
   - **Linker → System → Subsystem** → **Windows (/SUBSYSTEM:WINDOWS)**
4. Build (x64 recommended).
5. Run → Enjoy the process manager.

---

## 📸 UI Overview

- Search label + box (live filter)
- Buttons: Refresh, Kill, Suspend, Resume, Export JSON, Export CSV
- "Tree" checkbox to act recursively on children
- ListView with processes (PID, PPID, RSS, Name, Path)

---

## ⚠ Notes

- Some actions require Administrator rights (especially for system/privileged processes).
- `NtSuspendProcess` / `NtResumeProcess` are undocumented APIs available in `ntdll.dll`.
- Memory usage (RSS) is approximate.
- The app intentionally avoids background timers to remain responsive.

---

## 📜 License

MIT License — Free to use, modify, and distribute.

---

👤 **Author:** [Bob.Paydar@hp.com](mailto:Bob.Paydar@hp.com)
