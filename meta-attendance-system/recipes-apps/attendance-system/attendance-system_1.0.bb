SUMMARY = "PIR-triggered face attendance controller"
LICENSE = "CLOSED"

SRC_URI = "file://attendance-system.cpp file://attendance-system.service"
S = "${WORKDIR}"

inherit systemd

DEPENDS = "sqlite3"
RDEPENDS:${PN} = "attendance-ai tapo-rtsp pir-app pir-driver tft-app"
SYSTEMD_SERVICE:${PN} = "attendance-system.service"

do_compile() {
    ${CXX} ${CPPFLAGS} ${CXXFLAGS} -std=c++17 -Wall -Wextra -pthread \
        '-DBIN_DIR="${bindir}"' attendance-system.cpp ${LDFLAGS} -lsqlite3 -o attendance-system
}

do_install() {
    install -d ${D}${bindir} ${D}${systemd_system_unitdir}
    install -m 0755 attendance-system ${D}${bindir}/
    install -m 0644 attendance-system.service ${D}${systemd_system_unitdir}/
}
