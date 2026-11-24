SID Bridge @VERSION@
===================

Play the real SID chips in a C64 Ultimate from a MIDI keyboard or your DAW.

Install
-------
- The app: drag "SID Bridge.app" onto "Applications".
- The plug-ins (for a DAW): open "Plug-ins" and drag "SID Bridge.component" onto
  "Audio Unit folder" and "SID Bridge.vst3" onto "VST3 folder". macOS asks for your
  password because these are the plug-in folders for all users. Then restart your DAW
  (or rescan its plug-ins). SID Bridge is an instrument.

Before you start
----------------
- The C64 Ultimate must be on the same network as this Mac, with
  C64U menu > Network Settings > Web Remote Control switched on, and preferably
  Ultimate DMA Service too (faster; without it SID Bridge uses the REST API).
- The first time, macOS asks whether SID Bridge (or your DAW) may find devices on your
  local network: allow it, or SID Bridge can't reach the C64U.
- SID Bridge opens on its Setup tab and searches the network for your C64U by itself.

More: https://github.com/dantaipan/sid-bridge

SID Bridge is free software under the GNU AGPL 3.0 or later. It is built with JUCE
(https://juce.com), used under the AGPLv3.
