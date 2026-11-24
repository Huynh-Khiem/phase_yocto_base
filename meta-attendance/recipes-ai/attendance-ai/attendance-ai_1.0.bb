SUMMARY = "Tapo camera face recognition with YuNet and SFace ONNX models"
LICENSE = "CLOSED & MIT & Apache-2.0"
LIC_FILES_CHKSUM = "file://yunet-LICENSE;md5=2bab441cad6e56796bbbd90130807551 \
                   file://sface-LICENSE;md5=3b83ef96387f14655fc854ddc3c6bd57"

OPENCV_ZOO_REV = "48b10fa505aeb51fbd127fc60a5c660040e823b7"
OPENCV_ZOO_MODELS = "https://media.githubusercontent.com/media/opencv/opencv_zoo/${OPENCV_ZOO_REV}/models"
OPENCV_ZOO_LICENSES = "https://raw.githubusercontent.com/opencv/opencv_zoo/${OPENCV_ZOO_REV}/models"
SRC_URI = "file://attendance-ai.cpp \
           ${OPENCV_ZOO_MODELS}/face_detection_yunet/face_detection_yunet_2023mar.onnx;name=yunet \
           ${OPENCV_ZOO_MODELS}/face_recognition_sface/face_recognition_sface_2021dec.onnx;name=sface \
           ${OPENCV_ZOO_LICENSES}/face_detection_yunet/LICENSE;downloadfilename=yunet-LICENSE;name=yunet_license \
           ${OPENCV_ZOO_LICENSES}/face_recognition_sface/LICENSE;downloadfilename=sface-LICENSE;name=sface_license"
SRC_URI[yunet.sha256sum] = "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4"
SRC_URI[sface.sha256sum] = "0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79"
SRC_URI[yunet_license.sha256sum] = "c83b8120c50ccbd4c4f96edf53141bdd566ebb8f8e9227e415326aa1b1aba958"
SRC_URI[sface_license.sha256sum] = "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"
S = "${WORKDIR}"

DEPENDS = "onnxruntime"
RDEPENDS:${PN} = "onnxruntime tapo-rtsp"

do_compile() {
    ${CXX} ${CPPFLAGS} ${CXXFLAGS} -std=c++17 -Wall -Wextra -pthread \
        -I${STAGING_INCDIR}/onnxruntime '-DMODEL_DIR="${datadir}/attendance-ai"' \
        attendance-ai.cpp ${LDFLAGS} -lonnxruntime -o attendance-ai
}

do_install() {
    install -d ${D}${bindir} ${D}${datadir}/attendance-ai
    install -m 0755 attendance-ai ${D}${bindir}/
    install -m 0644 face_detection_yunet_2023mar.onnx face_recognition_sface_2021dec.onnx \
        yunet-LICENSE sface-LICENSE ${D}${datadir}/attendance-ai/
}

FILES:${PN} += "${datadir}/attendance-ai"
