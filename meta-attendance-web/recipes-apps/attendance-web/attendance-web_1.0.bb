SUMMARY = "Read-only web dashboard for the Pi attendance system"
LICENSE = "CLOSED"
SRC_URI = "file://attendance-web.cpp file://attendance-web.service \
           file://index.html file://dashboard.css file://dashboard.js"
S = "${WORKDIR}"

inherit systemd
DEPENDS = "libmicrohttpd sqlite3"
RDEPENDS:${PN} = "attendance-system"
SYSTEMD_SERVICE:${PN} = "attendance-web.service"

do_compile() {
    ${CXX} ${CPPFLAGS} ${CXXFLAGS} -std=c++17 -Wall -Wextra -pthread \
        '-DWEB_DIR="${datadir}/attendance-web"' attendance-web.cpp ${LDFLAGS} \
        -lmicrohttpd -lsqlite3 -o attendance-web
}

do_install() {
    install -d ${D}${bindir} ${D}${systemd_system_unitdir} ${D}${datadir}/attendance-web
    install -m 0755 attendance-web ${D}${bindir}/
    install -m 0644 attendance-web.service ${D}${systemd_system_unitdir}/
    install -m 0644 index.html dashboard.css dashboard.js ${D}${datadir}/attendance-web/
}

FILES:${PN} += "${datadir}/attendance-web"
