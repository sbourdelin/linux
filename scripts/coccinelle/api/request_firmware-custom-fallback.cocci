// Avoid the firmware custom fallback mechanism at all costs
//
// request_firmware_nowait() API enables explicit request for use of the custom
// fallback mechanism if firmware is not found. Chances are high its use is
// just a copy and paste bug. Before you fix the driver be sure to *verify* no
// custom firmware loading tool exists that would otherwise break if we replace
// the driver to use the uevent fallback mechanism.
//
// Confidence: High
//
// Reason for low confidence:
//
// Copyright: (C) 2016 Luis R. Rodriguez <mcgrof@kernel.org> GPLv2.
//
// Options: --include-headers

virtual report
virtual context

@ r0 depends on report || context @
declarer name DECLARE_FW_CUSTOM_FALLBACK;
expression E;
@@

DECLARE_FW_CUSTOM_FALLBACK(E);

@ r1 depends on report || context @
expression mod, name, dev, gfp, drv, cb;
position p;
@@

(
*request_firmware_nowait@p(mod, false, name, dev, gfp, drv, cb)
|
*request_firmware_nowait@p(mod, 0, name, dev, gfp, drv, cb)
|
*request_firmware_nowait@p(mod, FW_ACTION_NOHOTPLUG, name, dev, gfp, drv, cb)
)

@script:python depends on report && !r0 @
p << r1.p;
@@

coccilib.report.print_report(p[0], "WARNING: please check if driver really needs a custom fallback mechanism")
