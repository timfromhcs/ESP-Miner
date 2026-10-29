# Reporting Security Issues

We take security bugs in esp-miner seriously. We appreciate your efforts to responsibly disclose your findings, and will make every effort to acknowledge your contributions.

This repository (`timfromhcs/ESP-Miner`) is a fork with its own firmware releases. To report a security issue in this fork's code or releases, please use this repository's GitHub Security Advisory ["Report a Vulnerability"](https://github.com/timfromhcs/ESP-Miner/security/advisories/new) tab.

If the issue also affects the upstream project, it will be forwarded to the upstream maintainers via the [bitaxeorg/ESP-Miner advisory tab](https://github.com/bitaxeorg/ESP-Miner/security/advisories/new) after triage. You may also report it there directly.

A maintainer will send a response indicating the next steps in handling your report. After the initial reply to your report, we will keep you informed of the progress towards a fix and full announcement, and may ask for additional information or guidance.

Report security bugs in third-party modules to the person or team maintaining the module.

## Known security properties

- The AxeOS HTTP API, including `POST /api/system/OTA`, is plain HTTP without
  transport encryption or authentication. Access is limited only by the
  local-network check in `is_network_allowed()`. Never expose a miner's web
  interface to an untrusted network or the internet.
- OTA uploads are not signature-verified. The firmware checks that the upload
  is an ESP32-S3 application image of this project (image magic, chip id,
  application descriptor, project name), and ESP-IDF validates the image
  checksum before the slot is activated. Neither of these is authentication.
- Application rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) is not enabled
  in the shipped configuration. See `docs/OTA_SAFETY.md`.
