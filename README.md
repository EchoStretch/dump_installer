# Homebrew Dump Installer

Run homebrew apps on your PS5 using the Kstuff, Websrv payloads and Homebrew Launcher.

---

### ✅ Installation & Launch

1. **Copy your homebrew folder** to:
   - `/data/homebrew/`  
   - `/mnt/usb#/homebrew/` *(replace `#` with your USB number, e.g., `usb0`, `usb1`, etc.)*
   - `/mnt/ext#/homebrew/` *(replace `#` with your EXT number, e.g., `ext0`, `ext1`, etc.)*

2. **Inside each homebrew game folder**, add:
   - `dump_installer.elf`
   - `homebrew.js`
   - `sce_sys/icon0.png`

3. **Install the Homebrew Launcher** and send the Websrv payload to your console:  
   👉 [Websrv v0.28.3](https://github.com/ps5-payload-dev/websrv/releases/tag/v0.28.3)

4. **Open the Homebrew Launcher.**  
   With the USB plugged in, your homebrew Installer should appear.  
   **Navigate to the homebrew and install it.”**

---

### 📌 Example Folder Structure

`/data/homebrew/DumpInstaller/`, `/mnt/usb0/homebrew/DumpInstaller/` or `/mnt/ext0/homebrew/DumpInstaller/`
   - `dump_installer.elf`
   - `homebrew.js`
   - `sce_sys/icon0.png`

---
