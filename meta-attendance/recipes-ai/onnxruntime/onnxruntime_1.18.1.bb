SUMMARY = "ONNX Runtime CPU library for Linux AArch64"
HOMEPAGE = "https://onnxruntime.ai/"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=0f7e3b1308cb5c00b372a6e78835732d"

SRC_URI = "https://github.com/microsoft/onnxruntime/releases/download/v${PV}/onnxruntime-linux-aarch64-${PV}.tgz"
SRC_URI[sha256sum] = "c1dcd8ab29e8d227d886b6ee415c08aea893956acf98f0758a42a84f27c02851"

S = "${WORKDIR}/onnxruntime-linux-aarch64-${PV}"
COMPATIBLE_HOST = "aarch64.*-linux"

inherit bin_package

do_install() {
    install -d ${D}${libdir} ${D}${includedir}/onnxruntime ${D}${datadir}/${BPN}
    install -m 0755 ${S}/lib/libonnxruntime.so.${PV} ${D}${libdir}/
    ln -s libonnxruntime.so.${PV} ${D}${libdir}/libonnxruntime.so
    install -m 0644 ${S}/include/*.h ${D}${includedir}/onnxruntime/
    install -m 0644 ${S}/LICENSE ${S}/ThirdPartyNotices.txt ${D}${datadir}/${BPN}/
}

INSANE_SKIP:${PN} += "already-stripped"
