#include <netinet/in.h>
#include <stdbool.h>

struct addr_book;

struct addr_book *issue_network_gen_book(void);
void issue_network_free_book(struct addr_book *book);
const char *issue_network_get_best_ip(struct addr_book *book, const char *interface, bool ipv6);
