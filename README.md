# BLE Serial Bridge (NUS) für STM32WB5MM-DK

Transparente BLE-UART-Bridge auf Basis von Zephyr RTOS für das
[STM32WB5MM-DK](https://www.st.com/en/evaluation-tools/stm32wb5mm-dk.html)
Discovery Kit. Die Firmware stellt einen BLE-Peripheral mit den bekannten
**Nordic UART Service (NUS) UUIDs** bereit und leitet Daten bidirektional
zwischen BLE und einem freien UART des Boards weiter.

## Funktionsweise

| Richtung | Weg |
|---|---|
| UART → BLE | Daten am LPUART1-RX werden als Notifications auf der NUS-TX-Characteristic gesendet |
| BLE → UART | Writes auf die NUS-RX-Characteristic werden auf LPUART1-TX ausgegeben |

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

## Belegter UART / Verdrahtung

Als Bridge-UART wird **LPUART1** verwendet — der einzige frei nutzbare UART
des DK. USART1 (PB6/PB7) bleibt unangetastet, da er fest mit dem virtuellen
COM-Port des ST-LINK verbunden ist und weiter als Log-/Konsolenausgabe dient.

LPUART1 ist am **STMod+-Steckverbinder CN5** herausgeführt:

| Signal | MCU-Pin | STMod+ (CN5) | Anschluss Gegenstelle |
|---|---|---|---|
| LPUART1_TX | PA2 | Pin 2 (UART-TX lt. STMod+-Spezifikation TN1238) | RX des Partners |
| LPUART1_RX | PA3 | Pin 3 (UART-RX lt. STMod+-Spezifikation TN1238) | TX des Partners |
| GND | — | GND-Pin an CN5 (Belegung siehe UM2825, Tabelle „STMod+ connector") | GND des Partners |

Einstellungen: **115200 Baud, 8N1, keine Flusskontrolle**
(änderbar in `boards/stm32wb5mm_dk.overlay`). Pegel ist 3,3 V TTL.

## Voraussetzungen

- **Zephyr SDK 1.0.0** (oder neuer aus der 1.0.x-Reihe, GCC 14.2) —
  [Download](https://github.com/zephyrproject-rtos/sdk-ng/releases)
- **Zephyr v4.4.2** — wird über das enthaltene `west.yml` automatisch
  ausgecheckt (SDK 1.0.x ist offiziell kompatibel mit Zephyr 4.4.x)
- `west`, CMake ≥ 3.20, Ninja, Python 3 (siehe
  [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html))
- **STM32CubeProgrammer** zum Flashen der Funk-Firmware auf den CPU2

### Wichtig: BLE-Stack auf dem CPU2 (Cortex-M0+)

Der STM32WB ist ein Dual-Core: Zephyr läuft auf dem Cortex-M4 (CPU1), der
BLE-Controller auf dem Cortex-M0+ (CPU2). Auf dem CPU2 muss einmalig eine
passende ST-Coprozessor-Firmware installiert sein:

- Binary: `stm32wb5x_BLE_HCILayer_fw.bin` aus dem
  [STM32CubeWB-Paket](https://github.com/STMicroelectronics/STM32CubeWB)
  unter `Projects/STM32WB_Copro_Wireless_Binaries/STM32WB5x/`
- **Seit STM32CubeWB v1.13.2 funktioniert mit Zephyr nur noch die
  „HCI Layer"-Variante** — nicht die „Full Stack"-Variante!
- Installation per STM32CubeProgrammer über FUS; die zur Binary gehörende
  Start-Adresse steht in den Release Notes des Cube-Pakets
- Welche Cube-/Stack-Version zur verwendeten Zephyr-Version passt, steht in
  `modules/hal/stm32/lib/stm32wb/README.rst` im West-Workspace

## Bauen und Flashen

```bash
# Workspace anlegen (einmalig)
mkdir zephyr-workspace && cd zephyr-workspace
git clone https://github.com/protronic/BLEserial bleserial
west init -l bleserial
west update

# Bauen
west build -b stm32wb5mm_dk bleserial

# Flashen (CPU1, über den On-Board-ST-LINK)
west flash
```

## Benutzung

1. Gegenstelle (z. B. USB-UART-Adapter, 3,3 V!) an CN5 anschließen:
   TX↔RX kreuzen, GND verbinden; Terminal mit 115200 8N1 öffnen.
2. Mit einer NUS-fähigen App verbinden, z. B.:
   - **Serial Bluetooth Terminal** (Android)
   - **nRF Connect** / **nRF Toolbox → UART** (Android/iOS)
   - Gerätename: `BLESerial-WB5MM`
3. Notifications aktivieren (machen die genannten Apps automatisch) —
   ab dann werden Daten in beide Richtungen transparent durchgereicht.

Log-/Statusausgaben (Verbindungen, MTU, Pufferüberläufe) erscheinen auf dem
ST-LINK-VCP (USART1, 115200 8N1).

## Anpassungen

| Was | Wo |
|---|---|
| Baudrate / anderer UART | `boards/stm32wb5mm_dk.overlay` (`current-speed` bzw. Alias `bridge-uart`) |
| Gerätename | `prj.conf` → `CONFIG_BT_DEVICE_NAME` |
| MTU / Puffergrößen | `prj.conf` (`CONFIG_BT_L2CAP_TX_MTU`, ACL-Puffer) bzw. `src/main.c` (`*_BUF_SIZE`) |
