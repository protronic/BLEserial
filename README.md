# BLE Serial Bridge (NUS) für STM32WB / STM32WBA

Transparente BLE-UART-Bridge auf Basis von Zephyr RTOS. Die Firmware stellt
einen BLE-Peripheral mit den bekannten **Nordic UART Service (NUS) UUIDs**
bereit und leitet Daten bidirektional zwischen BLE und einem freien UART des
jeweiligen Boards weiter.

## Unterstützte Boards

| Board | Zephyr-Board | Bridge-UART | Bridge-Pins | Konsole/Log |
|---|---|---|---|---|
| [STM32WB5MM-DK](https://www.st.com/en/evaluation-tools/stm32wb5mm-dk.html) | `stm32wb5mm_dk` (upstream) | LPUART1 | PA2 = TX, PA3 = RX → STMod+ CN5 Pin 2/3 | USART1 → ST-LINK VCP |
| [STM32WBA55G-DK1](https://www.st.com/en/evaluation-tools/stm32wba55g-dk1.html) | `stm32wba55g_dk1` (eigene Board-Def.) | USART2 | PB0 = TX → ARDUINO D1, PA11 = RX → ARDUINO D0; auch STMod+ CN12 (JP2 = ON) | USART1 → ST-LINK VCP |
| [STM32WBA65I-DK1](https://www.st.com/en/evaluation-tools/stm32wba65i-dk1.html) | `stm32wba65i_dk1` (upstream) | USART2 | PA12 = TX → ARDUINO D1, PA11 = RX → ARDUINO D0 | USART1 → ST-LINK VCP |
| [B-WBA5M-WPAN](https://www.st.com/en/evaluation-tools/b-wba5m-wpan.html) (STM32WBA5MMG-Modul) | `b_wba5m_wpan` (eigene Board-Def.) | USART1 | PB12 = TX, PA8 = RX → M.2-E-Key-Hostanschluss | – (kein Log-UART) |

Alle UARTs laufen mit **115200 8N1, keine Flusskontrolle** (änderbar in den
Overlays unter `boards/`). Pegel 3,3 V TTL. TX/RX zur Gegenstelle kreuzen,
GND verbinden.

Die eigenen Board-Definitionen für STM32WBA55G-DK1 und B-WBA5M-WPAN liegen
unter `boards/st/` im Repo (in Zephyr 4.4 noch nicht upstream vorhanden) und
werden über `BOARD_ROOT` (im `CMakeLists.txt` gesetzt) automatisch gefunden.

Hinweis STM32WBA55G-DK1: Die Zuordnung D0 = PA11 (RX) stammt aus UM3255;
D1 = TX ist per Ausschlussverfahren PB0 (einzige freie USART2-TX-Option des
Gehäuses). Der Jumper JP2 wählt UART (ON) oder SPI für den STMod+. Bei
Problemen bitte gegen UM3255 (Arduino-/STMod+-Tabellen) prüfen.

## Funktionsweise

| Richtung | Weg |
|---|---|
| UART → BLE | Daten am Bridge-RX werden als Notifications auf der NUS-TX-Characteristic gesendet |
| BLE → UART | Writes auf die NUS-RX-Characteristic werden am Bridge-TX ausgegeben |

Verwendete UUIDs (Standard-NUS, kompatibel zu allen gängigen Apps):

| Funktion | UUID |
|---|---|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX (Write) | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX (Notify) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

Beide Richtungen sind über 2-KiB-Ringpuffer entkoppelt (interruptgesteuerter
UART-Betrieb). UART→BLE wird in Blöcken bis zur ausgehandelten ATT-MTU
(max. 247, also 244 Byte Nutzdaten pro Notification) gesendet; bei
Puffer-Engpässen im BLE-Host wird automatisch kurz verzögert wiederholt.
Nach einem Disconnect startet das Advertising automatisch neu.

## Voraussetzungen

- **Zephyr SDK 1.0.x** (GCC 14.2) —
  [Download](https://github.com/zephyrproject-rtos/sdk-ng/releases)
- **Zephyr v4.4.2** — wird über das enthaltene `west.yml` automatisch
  ausgecheckt
- `west`, CMake ≥ 3.20, Ninja, Python ≥ 3.12 (siehe
  [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html))
- **STM32CubeProgrammer** (Flash-Runner, beim WB5MM-DK zusätzlich für die
  CPU2-Funk-Firmware)

## Bauen und Flashen

```bash
# Workspace anlegen (einmalig)
mkdir zephyr-workspace && cd zephyr-workspace
git clone https://github.com/protronic/BLEserial bleserial
west init -l bleserial
west update

# Nur fuer die STM32WBA-Boards: BLE-Link-Layer-Bibliotheken (Binaer-Blobs)
west blobs fetch hal_stm32

# Bauen - Board nach Tabelle oben waehlen:
west build -b stm32wb5mm_dk    bleserial   # STM32WB5MM-DK
west build -b stm32wba55g_dk1  bleserial   # STM32WBA55G-DK1
west build -b stm32wba65i_dk1  bleserial   # STM32WBA65I-DK1
west build -b b_wba5m_wpan     bleserial   # B-WBA5M-WPAN (M.2-Modul)

# Flashen (ueber On-Board-ST-LINK bzw. SWD)
west flash
```

### Board-Besonderheiten

**STM32WB5MM-DK (Dual-Core):** Zephyr läuft auf dem Cortex-M4 (CPU1), der
BLE-Controller auf dem Cortex-M0+ (CPU2). Auf dem CPU2 muss einmalig die
passende ST-Coprozessor-Firmware installiert sein:
`stm32wb5x_BLE_HCILayer_fw.bin` aus dem
[STM32CubeWB-Paket](https://github.com/STMicroelectronics/STM32CubeWB)
(`Projects/STM32WB_Copro_Wireless_Binaries/STM32WB5x/`).
**Seit STM32CubeWB v1.13.2 funktioniert mit Zephyr nur die „HCI Layer"-,
nicht die „Full Stack"-Variante.** Installation per STM32CubeProgrammer/FUS;
passende Versionen siehe `modules/hal/stm32/lib/stm32wb/README.rst` im
Workspace.

**STM32WBA (Single-Core):** Kein Coprozessor — der BLE-Link-Layer wird als
Binärbibliothek (`west blobs fetch hal_stm32`) direkt eingelinkt. Die
Firmware nutzt die temperaturbasierte RF-Kalibrierung (ADC4 +
Die-Temperatursensor, in den Board-Definitionen bereits aktiviert).

**B-WBA5M-WPAN:** Die M.2-Karte hat keinen eigenen ST-LINK; programmiert
wird über SWD vom Host-Board (siehe UM3450). Da USART1 als Bridge zum
M.2-Host dient, ist das Logging auf diesem Board deaktiviert
(`boards/b_wba5m_wpan.conf`).

## Benutzung

1. Gegenstelle (z. B. USB-UART-Adapter, 3,3 V!) an die Bridge-Pins laut
   Tabelle anschließen: TX↔RX kreuzen, GND verbinden; Terminal mit
   115200 8N1 öffnen. Beim B-WBA5M-WPAN übernimmt das Host-Board über den
   M.2-Anschluss die UART-Seite.
2. Mit einer NUS-fähigen App verbinden, z. B.:
   - **Serial Bluetooth Terminal** (Android)
   - **nRF Connect** / **nRF Toolbox → UART** (Android/iOS)
   - Gerätename: `BLESerial-WB5MM` (änderbar in `prj.conf`)
3. Notifications aktivieren (machen die genannten Apps automatisch) —
   ab dann werden Daten in beide Richtungen transparent durchgereicht.

Log-/Statusausgaben (Verbindungen, MTU, Pufferüberläufe) erscheinen auf dem
ST-LINK-VCP (115200 8N1) — außer beim B-WBA5M-WPAN.

## Anpassungen

| Was | Wo |
|---|---|
| Baudrate / anderer UART | `boards/<board>.overlay` (`current-speed` bzw. Alias `bridge-uart`) |
| Gerätename | `prj.conf` → `CONFIG_BT_DEVICE_NAME` |
| MTU / Puffergrößen | `prj.conf` (`CONFIG_BT_L2CAP_TX_MTU`, ACL-Puffer) bzw. `src/main.c` (`*_BUF_SIZE`) |
| Eigene Boards | `boards/st/stm32wba55g_dk1/`, `boards/st/b_wba5m_wpan/` |
