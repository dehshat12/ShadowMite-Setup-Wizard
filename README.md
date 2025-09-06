# ShadowMite Setup Wizard

**ShadowMite** is a GTK3-based setup wizard written in C++ designed to guide users through configuring a fresh OS environment. Its sleek, stack-based interface allows easy configuration of network, locale, applications, and system settings. ShadowMite is ideal for customized Raspberry Pi or kiosk setups.

Notes: The templates folder contains the app jsons

---

## Features

- **Welcome Screen** – the entry point to the setup process.  
- **Network Setup** – Wi-Fi scanning, static IP configuration, and asynchronous threads for reliability.  
- **Locale Selection** – select language and region.  
- **Application Manager** – install/remove “prescribed apps” from JSON definitions.  
- **Summary & Finish** – review and confirm configuration choices.

## Notes: 
The templates folder contains the app jsons, so if you dont see any apps in the setup wizard, don't panic. You can make your own, or copy the entire files to the ~/sm_conf/apps/. 

---


Building ShadowMite is as easy. Get your Raspberry Pi Lite OS flashed into an SD card and follow the rest
---
Install the required libraries for compiling:

```bash
sudo apt update
sudo apt install build-essential pkg-config libgtk-3-dev

`````
---
Clone the repository
```bash
git clone https://github.com/dehshat12/ShadowMite-Setup-Wizard.git
cd ShadowMite
make 
`````
---

Then Execute

```bash
./ShadowMite 
`````
---

# ⚠️WARNING⚠️

The Program is still in development, so expect some missing key features, bugs and glitches. Use it at your own risk

