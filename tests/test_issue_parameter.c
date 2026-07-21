#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "issue.h"

int main(void)
{
	static const char issue[] = "\\e{color-name-that-exceeds-buffer}suffix\n";
	static const char expected[] = "\033suffix\r\n";
	char path[] = "/tmp/kmscon-test-issue-parameter-XXXXXX";
	char *buf;
	size_t buf_len = 0;
	ssize_t len;
	int fd;

	fd = mkstemp(path);
	if (fd < 0) {
		perror("mkstemp");
		return EXIT_FAILURE;
	}

	len = write(fd, issue, sizeof(issue) - 1);
	close(fd);
	if (len < 0 || (size_t)len != sizeof(issue) - 1) {
		perror("write");
		unlink(path);
		return EXIT_FAILURE;
	}

	buf = kmscon_issue_get_buffer(path, "faketty1", &buf_len);
	unlink(path);
	if (!buf || buf_len != sizeof(expected) - 1 ||
	    memcmp(buf, expected, sizeof(expected) - 1)) {
		fprintf(stderr, "long parameter expansion failed\n");
		free(buf);
		return EXIT_FAILURE;
	}

	free(buf);
	return EXIT_SUCCESS;
}
