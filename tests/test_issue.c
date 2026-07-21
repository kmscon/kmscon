#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "issue.h"
#include "issue_network.h"

struct kmscon_pty;

static int test_missing_interface(void)
{
	static const char issue[] = "\\4{no-such-if}\n";
	char path[] = "/tmp/kmscon-test-issue-XXXXXX";
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
	if (!buf || buf_len != 2 || memcmp(buf, "\r\n", 2)) {
		fprintf(stderr, "missing-interface expansion failed\n");
		free(buf);
		return EXIT_FAILURE;
	}

	free(buf);
	return EXIT_SUCCESS;
}

int main()
{
	char *search_path = ISSUE_DEFAULT_PATH;
	const char *ip;
	size_t buf_len;
	struct addr_book *book;
	char *buf = kmscon_issue_get_buffer(search_path, "faketty1", &buf_len);

	if (test_missing_interface())
		return EXIT_FAILURE;

	printf("%s\n", buf);
	free(buf);

	printf("----- check network issue -----\n");

	book = issue_network_gen_book();

	ip = issue_network_get_best_ip(book, NULL, false);
	printf("ipv4: %s\n", ip ? ip : "(none)");
	ip = issue_network_get_best_ip(book, NULL, true);
	printf("ipv6: %s\n", ip ? ip : "(none)");
	printf("good ips: %s\n", issue_network_get_all_ip(book, true));
	printf("all ips: %s\n", issue_network_get_all_ip(book, false));

	issue_network_free_book(book);
	return 0;
}
