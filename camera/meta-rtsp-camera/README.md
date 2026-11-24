# Camera Tapo C210 trên Yocto Kirkstone

Layer cho Raspberry Pi 4 64-bit với image riêng `core-image-rtsp-camera`. App độc lập
nhận H.264 và ghi log số frame, FPS thực nhận và khoảng cách giữa hai frame mới nhất.

## Build

Từ thư mục workspace chứa `poky`, `meta-raspberrypi`, `meta-openembedded` và layer này:

```bash
TEMPLATECONF="$PWD/meta-rtsp-camera/conf/templates/default" \
  source poky/oe-init-build-env build-rtsp-camera
bitbake core-image-rtsp-camera
```

BitBake dùng lại thư mục download và sstate của `build/`. Hai file cấu hình mẫu nằm trong
`conf/templates/default/`. Nếu đã có `build-rtsp-camera/conf/`, script giữ cấu hình
hiện có. File để ghi lên thẻ SD nằm tại
`build-rtsp-camera/tmp/deploy/images/raspberrypi4-64/core-image-rtsp-camera-raspberrypi4-64.wic.bz2`.
Có thể giải nén bằng `bunzip2 -k` và chọn file `.wic` trong công cụ ghi thẻ SD.

Nếu dùng build hiện có với `core-image-minimal`, thêm layer vào `bblayers.conf`
và dùng các dòng sau trong `local.conf`. App cần image chạy systemd và NetworkManager
cần tính năng distro `wifi` (Poky bật sẵn).

```bitbake
IMAGE_INSTALL:append:pn-core-image-minimal = " camera-wifi tapo-rtsp iw"
LICENSE_FLAGS_ACCEPTED:append = " commercial_ffmpeg commercial_gstreamer1.0-libav"
```

Sau đó chạy `bitbake core-image-minimal` và ghi file `core-image-minimal-raspberrypi4-64.wic.bz2` mới lên thẻ.

## Cài Wi-Fi và camera

Đăng nhập `root` từ console của Pi. Image thử nghiệm cho phép đăng nhập console với mật khẩu
root trống lúc đầu và không cài SSH. Đặt mật khẩu bằng `passwd` sau lần đăng nhập đầu tiên.

```sh
nmcli radio wifi on
nmcli device status
nmcli device wifi list
nmcli --ask device wifi connect 'TEN_WIFI'
nmcli connection modify 'TEN_WIFI' connection.autoconnect yes connection.autoconnect-retries 0
```

Lệnh kết nối lưu profile trên Pi để NetworkManager nối lại khi khởi động hoặc khi
Wi-Fi trở lại. Nếu tên profile khác tên SSID, lấy tên profile bằng `nmcli connection show`.

Nếu `nmcli` báo `No Wi-Fi device found`, kiểm tra driver và interface trước khi nhập SSID:

```sh
nmcli device status
ip link
modprobe brcmfmac
dmesg | grep -Ei 'brcmfmac|firmware|cfg80211'
```

Trên router, tạo DHCP reservation theo MAC của Tapo C210 và dùng địa chỉ đã đặt trong
file cấu hình bên dưới. Camera tiếp tục dùng DHCP và luôn được router cấp cùng một IP.
Trong Tapo app, tạo **Camera Account** cho RTSP. Xem [hướng dẫn TP-Link](https://www.tp-link.com/us/support/faq/2680/).

```sh
cp /etc/tapo-rtsp.conf.example /etc/tapo-rtsp.conf
chmod 600 /etc/tapo-rtsp.conf
vi /etc/tapo-rtsp.conf
systemctl restart tapo-rtsp
journalctl -u tapo-rtsp -f
```

Bắt đầu với `stream2` (luồng nhẹ hơn) và UDP, FPS do camera phát. Đặt IP đã dành riêng
trong `url`. Nếu mạng có nhiều mất gói, có thể thử `transport=tcp` và đo lại.
Sau khi chỉnh cấu hình, khởi động lại service.

## Đọc mã

- `tapo-rtsp.c` tạo pipeline GStreamer, lọc luồng video H.264, tháo RTP, parse H.264,
  giải mã bằng `avdec_h264` và đưa frame GRAY8 vào appsink. Bộ đệm RTP mặc định
  là 35 ms, appsink giữ tối đa một frame và bỏ frame cũ khi app đọc chậm.
- Đặt `ai-frames=true` trong mục `[camera]` của `/etc/tapo-rtsp.conf` và khởi động lại
  service để appsink nhận RGB. Camera thay thế nguyên tử file PPM mới nhất ở
  `/run/tapo-rtsp/latest.ppm`, kèm thời điểm nhận frame theo microgiây Unix.
  `attendance-ai` đọc file này và dùng kết nối RTSP đang chạy của camera.
- Vòng lặp đợi frame từng đợt tối đa 100 ms. App thoát với mã lỗi khi gặp lỗi pipeline,
  hết stream, sau 12 giây không có frame đầu tiên hoặc 4 giây không có frame mới.
  systemd thử chạy lại sau 2 giây. Watchdog 20 giây xử lý trường hợp pipeline kẹt
  và vòng lặp ngừng báo trạng thái. NetworkManager quản lý kết nối Wi-Fi riêng.
- Recipe `tapo-rtsp` biên dịch app, cài plugin và service. Recipe `camera-wifi`
  cài NetworkManager, hỗ trợ Wi-Fi và firmware. `core-image-rtsp-camera.bb` gom hai recipe vào image.

## Kiểm thử trên Pi

1. Chạy camera trong 10 phút. Kiểm tra log có FPS ổn định theo tốc độ luồng, số frame
   tăng đều và không có lần khởi động lại bất ngờ.
2. Tắt nguồn camera ít nhất 30 giây rồi bật lại. Kiểm tra log báo lỗi hoặc timeout,
   service thử lại và có dòng `First decoded frame` sau khi camera quay lại.
3. Tắt Wi-Fi của Pi bằng `nmcli radio wifi off`. Sau 30 giây, bật lại bằng
   `nmcli radio wifi on`. Kiểm tra Pi tự kết nối profile và app tự nhận frame.
4. Để kiểm tra watchdog riêng, khi stream đang chạy dùng
   `systemctl kill --signal=SIGSTOP tapo-rtsp`. Kiểm tra journal có lần khởi động lại
   do watchdog và số frame bắt đầu tăng lại.

### Đo từ lúc ghi hình đến lúc app nhận frame

1. Nối màn hình HDMI vào Pi, chạy `tapo-rtsp --clock` từ console và hướng C210 vào
   các chữ số. Đồng hồ hiện Unix timestamp theo microgiây, cập nhật khoảng mỗi 10 ms.
2. Đặt `snapshot=true` trong cấu hình và chạy `systemctl restart tapo-rtsp`.
   Dùng console thứ hai hoặc cổng serial để lưu các bản sao của
   `/run/tapo-rtsp/latest.pgm` cách nhau hơn 2 giây. Sau khi file xuất hiện, có thể
   lưu 30 mẫu liên tiếp bằng lệnh sau:

   ```sh
   mkdir -p /tmp/rtsp-samples
   for i in $(seq 1 30); do
       cp /run/tapo-rtsp/latest.pgm "/tmp/rtsp-samples/frame-$i.pgm"
       sleep 3
   done
   ```

3. Mở ảnh PGM trên máy có trình xem ảnh và đọc số đồng hồ xuất hiện trong ảnh.
   Dùng `head -n 2 ten-anh.pgm` để lấy `received_unix_us`, thời điểm ngay sau
   khi appsink trả frame. Lấy hiệu hai số rồi chia cho 1.000 để ra ms.
4. Lấy ít nhất 30 mẫu ở điều kiện Wi-Fi sử dụng thực tế. Ghi median, p95, max và
   số mẫu vượt 150 ms. Độ trễ hiển thị của màn hình cộng thêm sai số vào phép đo.
   Nếu kết quả sát ngưỡng, cần hiệu chuẩn phần hiển thị hoặc dùng mốc quang có timestamp.
   Tắt snapshot sau khi đo.

Cột `latest_gap_ms` chỉ là khoảng cách giữa hai lần app nhận frame. Mốc 150 ms
bao gồm phơi sáng, xử lý và mã hóa trong camera, Wi-Fi, bộ đệm và giải mã trên Pi.
Cần số đo thực tế với C210 để xác nhận đạt mốc này.
