SUMMARY = "Userspace test application for ST7735R TFT display with auto-reconnect"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://tft_test.c"

S = "${WORKDIR}"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} ${WORKDIR}/tft_test.c -o tft-test
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 tft-test ${D}${bindir}/tft-test
}

# Tu dong keo theo kernel module st7735r (va cac module DRM phu thuoc) vao rootfs
RRECOMMENDS:${PN} += "kernel-module-st7735r"
