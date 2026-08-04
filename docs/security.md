# Security posture

Wi-Fi and the HTTP control plane are disabled by default in every tracked RF3
firmware environment. Local credentials belong in the ignored
include/wifi_control_config.local.hpp file or local build macros. The tracked
example contains placeholders only.

The historical repository contained a real SSID and password. Removing them
from the current tree does not remove them from Git history. Treat those values
as exposed and rotate the network password; this change intentionally does not
rewrite shared history.

If explicitly enabled, the current HTTP endpoints have no authentication,
authorization, confidentiality, request signing, or cross-site request
protection. They can start transmission and change radio/application state.
Enable them only on an isolated development network and never expose the
device directly to an untrusted LAN or the internet.

Protocol v2 provides accidental-corruption detection with CRC32, not
cryptographic authenticity or secrecy. A peer that can transmit on the radio
channel can spoof, observe, interrupt, or replace a transfer.
