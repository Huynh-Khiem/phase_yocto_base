SUMMARY = "Raspberry Pi console image for the standalone Tapo RTSP receiver"
LICENSE = "MIT"

inherit core-image
# Allow initial setup from the physical console on the test image.
IMAGE_FEATURES += "empty-root-password"

IMAGE_INSTALL += "camera-wifi tapo-rtsp iw"
