#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "issue.h"

struct kmscon_pty;

int main()
{
	char *search_path = ISSUE_DEFAULT_PATH;
	size_t buf_len;

	char *buf = kmscon_issue_get_buffer(search_path, "faketty1", &buf_len);

	printf("%s\n", buf);
	free(buf);

	return 0;
}
