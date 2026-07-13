#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "issue.h"
#include "issue_network.h"

struct kmscon_pty;

int main()
{
	char *search_path = ISSUE_DEFAULT_PATH;
	size_t buf_len;
	struct addr_book *book;
	char *buf = kmscon_issue_get_buffer(search_path, "faketty1", &buf_len);

	printf("%s\n", buf);
	free(buf);

	printf("----- check network issue -----\n");

	book = issue_network_gen_book();

	printf("ipv4: %s\n", issue_network_get_best_ip(book, NULL, false));
	printf("ipv6: %s\n", issue_network_get_best_ip(book, NULL, true));
	printf("good ips: %s\n", issue_network_get_all_ip(book, true));
	printf("all ips: %s\n", issue_network_get_all_ip(book, false));

	issue_network_free_book(book);
	return 0;
}
