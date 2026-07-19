.. zephyr:board:: esp32p4_wifi6

Overview
********

The Waveshare ESP32-P4-WIFI6 is a multimedia development board built around the
ESP32-P4 SoC. It stacks 32 MB of PSRAM and connects a 32 MB NOR flash, exposes
a Type-C USB-to-UART port for flashing and console, a 4-pin USB-OTG HS
connector, MIPI CSI/DSI FPC connectors, a MicroSD slot, and a 40-pin GPIO
header compatible with some Raspberry Pi HATs. Wireless connectivity is
provided by an on-board ESP32-C6-MINI-1 module connected over SDIO.

This board definition targets the ESP32-P4 high-performance (HP) core and
routes the Zephyr console to UART0 (GPIO37/38), which is the port wired to the
Type-C USB-to-UART bridge.

Hardware
********

- ESP32-P4 SoC (silicon revision v1.3) with 32 MB stacked PSRAM and 32 MB
  on-board NOR flash
- Type-C USB-to-UART port (CH343P) for power, flashing and serial console
- 4-pin USB 2.0 OTG HS connector
- 2-lane MIPI CSI camera connector
- 2-lane MIPI DSI display connector
- MicroSD card slot (SDIO 3.0: clk=39, cmd=43, d0=40, d1=44, d2=41, d3=42)
- 40-pin GPIO expansion header
- Boot (GPIO0) and reset buttons

The default partition table is extended so the ``storage`` partition occupies
the space between the standard 16 MB layout and the ``image-scratch`` /
``coredump`` partitions, which are moved to the end of the 32 MB flash.
MIPI DSI/CSI, I2S audio and the ESP32-C6 SDIO wireless co-processor are not
enabled in this initial board port.

Supported Features
==================

.. zephyr:board-supported-hw::

Supported Runners
=================

.. zephyr:board-supported-runners::

Programming and Debugging
*************************

Build and flash the sample as follows:

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: esp32p4_wifi6/esp32p4/hpcore
   :goals: build flash
   :compact:

The board is flashed with the ``esp32`` runner via the Type-C USB-to-UART
bridge. If the serial port is not auto-detected, pass it explicitly::

   west flash --runner esp32 --esp-device /dev/ttyACM0

After flashing, open a serial terminal on the same port at 115200 bps::

   picocom -b 115200 /dev/ttyACM0

The LP core target (``esp32p4_wifi6/esp32p4/lpcore``) is also available.
It uses the LP UART with the default pins (GPIO14/15) for the console; to
flash it, use the same ``esp32`` runner and explicitly select the target::

   west build -b esp32p4_wifi6/esp32p4/lpcore samples/hello_world
   west flash --runner esp32 --esp-device /dev/ttyACM0

References
**********

.. target-notes::

.. _`Waveshare ESP32-P4-WIFI6 Wiki`: https://www.waveshare.com/wiki/ESP32-P4-WIFI6
.. _`Waveshare ESP32-P4-WIFI6 Schematic`: https://files.waveshare.com/wiki/ESP32-P4-WIFI6/ESP32-P4-WIFI6-datasheet.pdf
