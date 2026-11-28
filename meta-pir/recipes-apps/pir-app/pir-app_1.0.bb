SUMMARY = "Userspace test application for PIR sensor driver"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://user.c"

S = "${WORKDIR}"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} ${WORKDIR}/user.c -o pir-test
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 pir-test ${D}${bindir}/pir-test
}
