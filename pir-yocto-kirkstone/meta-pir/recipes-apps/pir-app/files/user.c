#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define DEVICE_PATH "/dev/m_device"

int main(void)
{
	int fd;
	char buf[128];
	ssize_t n;

	fd = open(DEVICE_PATH, O_RDONLY);
	if (fd < 0) {
		perror("Khong the mo " DEVICE_PATH);
		return EXIT_FAILURE;
	}

	printf("Dang cho du lieu tu %s...\n", DEVICE_PATH);

	while (1) {
		n = read(fd, buf, sizeof(buf) - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("Loi doc du lieu");
			close(fd);
			return EXIT_FAILURE;
		}
		buf[n] = '\0';
		printf("%s", buf);
		fflush(stdout);
	}

	close(fd);
	return EXIT_SUCCESS;
}
