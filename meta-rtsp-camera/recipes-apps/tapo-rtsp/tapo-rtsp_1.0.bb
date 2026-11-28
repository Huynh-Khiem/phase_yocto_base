SUMMARY = "Standalone H.264 RTSP receiver with frame logging and watchdog"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://tapo-rtsp.c file://tapo-rtsp.service file://tapo-rtsp.conf.example"
S = "${WORKDIR}"

inherit pkgconfig systemd

DEPENDS = "gstreamer1.0 gstreamer1.0-plugins-base gstreamer1.0-plugins-good systemd"
RDEPENDS:${PN} = " \
    gstreamer1.0-plugins-base-app \
    gstreamer1.0-plugins-base-videoconvert \
    gstreamer1.0-plugins-good-rtsp \
    gstreamer1.0-plugins-good-rtp \
    gstreamer1.0-plugins-good-rtpmanager \
    gstreamer1.0-plugins-good-udp \
    gstreamer1.0-plugins-bad-videoparsersbad \
    gstreamer1.0-libav \
"

SYSTEMD_SERVICE:${PN} = "tapo-rtsp.service"

do_compile() {
    ${CC} ${CPPFLAGS} ${CFLAGS} -Wall -Wextra tapo-rtsp.c ${LDFLAGS} \
        $(pkg-config --cflags --libs gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-rtsp-1.0 libsystemd) \
        -o tapo-rtsp
}

do_install() {
    install -d ${D}${bindir} ${D}${systemd_system_unitdir} ${D}${sysconfdir}
    install -m 0755 tapo-rtsp ${D}${bindir}/
    install -m 0644 tapo-rtsp.service ${D}${systemd_system_unitdir}/
    install -m 0600 tapo-rtsp.conf.example ${D}${sysconfdir}/
}

FILES:${PN} += "${sysconfdir}/tapo-rtsp.conf.example"
