SUMMARY = "NetworkManager Wi-Fi support for the Raspberry Pi camera receiver"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://90-camera-wifi.conf"
S = "${WORKDIR}"

RDEPENDS:${PN} = "networkmanager networkmanager-nmcli networkmanager-wifi wpa-supplicant"
PACKAGE_ARCH = "${MACHINE_ARCH}"
RDEPENDS:${PN}:append:raspberrypi4 = " \
    kernel-module-brcmfmac \
    linux-firmware-rpidistro-bcm43455 \
    linux-firmware-rpidistro-bcm43456 \
    wireless-regdb \
"

do_install() {
    install -d ${D}${sysconfdir}/NetworkManager/conf.d
    install -m 0644 ${WORKDIR}/90-camera-wifi.conf ${D}${sysconfdir}/NetworkManager/conf.d/
}
