SIDwire @VERSION@
================

Play the real SID chips in a C64 Ultimate from a MIDI keyboard or your DAW.

Install
-------
- The app: drag "SIDwire.app" onto "Applications".
- The plug-ins (for a DAW): open "Plug-ins" and drag "SIDwire.component" onto
  "Audio Unit folder" and "SIDwire.vst3" onto "VST3 folder". macOS asks for your
  password because these are the plug-in folders for all users. Then restart your DAW
  (or rescan its plug-ins). SIDwire is an instrument.

Upgrading from SID Bridge
-------------------------
SIDwire used to be called SID Bridge (up to version 0.1.1). After installing SIDwire,
delete the old copies: "SID Bridge.app" from Applications, and "SID Bridge.component"
and "SID Bridge.vst3" from the Audio Unit and VST3 folders. With both installed your
DAW may load the old one. Your settings and controller profiles carry over to SIDwire
by themselves, and so do DAW projects in most DAWs (a host that remembers a VST3 by its
file path may ask for "SID Bridge.vst3": rescan and load SIDwire there instead).

Before you start
----------------
- The C64 Ultimate must be on the same network as this Mac, with
  C64U menu > Network Settings > Web Remote Control switched on, and preferably
  Ultimate DMA Service too (faster; without it SIDwire uses the REST API).
- The first time, macOS asks whether SIDwire (or your DAW) may find devices on your
  local network: allow it, or SIDwire can't reach the C64U.
- SIDwire opens on its Setup tab and searches the network for your C64U by itself.

More: https://github.com/dantaipan/sidwire

SIDwire is free software under the GNU AGPL 3.0 or later. It is built with JUCE
(https://juce.com), used under the AGPLv3.
