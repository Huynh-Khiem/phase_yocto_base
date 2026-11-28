FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://pir-overlay.dts"

do_compile:append() {
    DTC_BIN="${B}/scripts/dtc/dtc"
    if [ ! -x "${DTC_BIN}" ]; then
        DTC_BIN="dtc"
    fi
    ${DTC_BIN} -@ -I dts -O dtb -o ${B}/pir-overlay.dtbo ${WORKDIR}/pir-overlay.dts
}

do_deploy:append() {
    install -d ${DEPLOYDIR}/overlays
    install -m 0644 ${B}/pir-overlay.dtbo ${DEPLOYDIR}/overlays/pir-overlay.dtbo
}
