FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += " \
    file://st7735r-overlay.dts \
    file://st7735r.cfg \
"

KERNEL_MODULE_AUTOLOAD:append = " st7735r"

do_compile:append() {
    DTC_BIN="${B}/scripts/dtc/dtc"
    if [ ! -x "${DTC_BIN}" ]; then
        DTC_BIN="dtc"
    fi
    ${DTC_BIN} -@ -I dts -O dtb -o ${B}/st7735r-overlay.dtbo ${WORKDIR}/st7735r-overlay.dts
}

do_deploy:append() {
    install -d ${DEPLOYDIR}/overlays
    install -m 0644 ${B}/st7735r-overlay.dtbo ${DEPLOYDIR}/overlays/st7735r-overlay.dtbo
}
