### Linux (via Wine)

Debian example:

```
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install wine32 winetricks
winetricks d3dcompiler_47
winetricks d3dx9_36
PULSE_LATENCY_MSEC=10 wine milkbottle.exe
```

Use Bullseye or later because _CoCreateInstance_ fails with _REGDB_E_CLASSNOTREG_ for _MMDeviceEnumerator_ (bcde0395-e52f-467c-8e3d-c4579291692e) on Buster.
