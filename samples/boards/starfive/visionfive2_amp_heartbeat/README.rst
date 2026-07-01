.. zephyr:code-sample:: visionfive2_amp_heartbeat
   :name: VisionFive 2 AMP heartbeat, logging, and shell

   Run Zephyr on the JH7110 S7 hart alongside Linux on the U74 harts.

Overview
********

This sample is loaded by the VisionFive 2 AMP-capable SPL over XMODEM. Zephyr
runs in M-mode on S7 hart 0 while the QSPI firmware payload starts Linux on
U74 harts 1 through 4.

The sample keeps UART0 available to Linux and provides these shared-memory
interfaces instead:

* A heartbeat at ``0x6e400000``.
* A 64 KiB Zephyr log ring at ``0x6e401000``.
* A bidirectional Zephyr shell transport at ``0x6e412000``.

The complete ``0x6e400000`` through ``0x6fffffff`` AMP region must be reserved
from Linux. The matching U-Boot branch adds this reserved-memory region to the
Linux devicetree during boot.

Building
********

Build the sample from the west workspace root:

.. code-block:: console

   west build -p always -b visionfive2/jh7110 \
     zephyr/samples/boards/starfive/visionfive2_amp_heartbeat

Send ``build/zephyr/zephyr.bin`` when SPL requests the UART XMODEM payload.

Linux tools
***********

The tools directory contains readers that access the reserved region through
``/dev/mem`` and therefore must run as root.

Capture Zephyr logs independently of the interactive shell:

.. code-block:: console

   python3 tools/vf2-zephyr-log.py

Enter the Zephyr shell through the shared-memory transport:

.. code-block:: console

   python3 tools/vf2-zephyr-shell.py

Press ``Ctrl-]`` to return to the Linux shell. Useful Zephyr commands include
``kernel threads``, ``device list``, ``amp status``, ``amp log status``, and
``amp log test``. Shell output and logging use separate rings, so Linux can
continue capturing logs while the interactive shell is active.
