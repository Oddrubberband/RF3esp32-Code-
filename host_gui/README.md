# RF3 Desktop

The maintained GUI is `dist/RF3Desktop/RF3Desktop.exe`, built with RF3Desktop.spec.
Use the root README.md and docs/build_and_test.md for portable commands. Complete
prebuilt GUI distributions are attached to the current GitHub release after CI.

```powershell
py -3.11 -m venv .venv-host
.\.venv-host\Scripts\python.exe -m pip install -r host_gui\requirements.txt
.\.venv-host\Scripts\python.exe host_gui\run_gui.py --backend mock
.\.venv-host\Scripts\python.exe host_gui\run_gui.py --backend serial
.\.venv-host\Scripts\python.exe -m PyInstaller --noconfirm RF3Desktop.spec
.\Launch_GUI.cmd
```

Classic SPP discovery binds the selected Bluetooth address to its unique outgoing
COM port. It does not select an incoming or unrelated port. HELLO identifies RF3
independently of radio readiness; confirmed RF controls stay guarded until status
arrives. A missing radio does not itself invalidate the PC-to-ESP link.

The SPIFFS tab lists/imports/selects/transmits/exports/deletes stored files. PC
source, normalized upload name, ESP selected file and current/last RF source are
shown separately. Host imports verify CRC and every byte by readback; exports
publish atomically only after CRC/size checks. Status refresh preserves unsent
channel/power drafts. Incoming progress/rate is separate from outgoing transfers.

Each GUI instance uses a separate diagnostic log, whose location is displayed.
File payloads and live-message contents are omitted from saved diagnostics.
A debug RX arrow means the PC received text, not proof of RF peer delivery.
Both boards need current firmware for suffix metadata; see docs/protocol_v2.md.
