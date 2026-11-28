SUMMARY = "PIR sensor platform kernel module with Device Tree & debounce"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module

SRC_URI = " \
    file://Makefile \
    file://pir.c \
"

S = "${WORKDIR}"

# Tu dong load module khi boot
KERNEL_MODULE_AUTOLOAD += "pir"
