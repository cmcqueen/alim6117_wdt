# alim6117_wdt
Linux kernel driver for ALi M6117 watchdog device

This provides an out-of-tree kernel driver for the ALI M6117 watchdog hardware, as used in DM&P eBox mini-PCs.

It is based on a driver originally written by Federico Bareilles, found here:

* [ALi M6117 Watchdog timer driver for Linux 2.4.x](http://www.iar.unlp.edu.ar/~fede/ali_m6117.html)
    * [Source code `alim6117_wdt.c`](http://www.iar.unlp.edu.ar/~fede/pub/kernel/alim6117_wdt/alim6117_wdt.c)

That driver didn't compile with more recent kernels, though. So it has been updated to compile with more recent kernels
(tested with 3.16.x kernel). It has also been converted to use the kernel watchdog API, which simplifies the code.
