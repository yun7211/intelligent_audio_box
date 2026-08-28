# ESP32-S3 16 MB v2 Partition Table

This repository keeps one partition layout: `16m.csv`. It is shared by the supported ESP-BOX-3 and LiChuang ESP32-S3 boards.

| Partition | Size | Purpose |
| --- | ---: | --- |
| `nvs` | 16 KB | Non-volatile settings |
| `otadata` | 8 KB | OTA selection metadata |
| `phy_init` | 4 KB | PHY initialization data |
| `ota_0` | ~4.06 MB (`0x410000`) | First application slot |
| `ota_1` | ~4.06 MB (`0x410000`) | Second application slot |
| `assets` | Remaining flash (~7.75 MB, `0x7c0000`) | Models, fonts, sounds, images, and UI resources |

The dual application slots support safe OTA replacement: a new image is written to the inactive slot and selected only after the update succeeds. The `assets` partition is managed independently so models and UI resources can change without replacing the application image.

The old v1 layout and the removed 4 MB, 8 MB, ESP32-C3, and 32 MB variants are not supported by this focused ESP32-S3 repository.
