/*
 * kmscon - asciicast playback for terminal sessions
 */

#ifndef ASCIINEMA_H
#define ASCIINEMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ev_eloop;
struct kmscon_asciinema;

typedef void (*kmscon_asciinema_write_cb)(const char *u8, size_t len, void *data);

int kmscon_asciinema_new(struct kmscon_asciinema **out, struct ev_eloop *eloop, const char *path,
			 bool loop, kmscon_asciinema_write_cb write_cb, void *data);
void kmscon_asciinema_free(struct kmscon_asciinema *player);

bool kmscon_asciinema_start(struct kmscon_asciinema *player);
bool kmscon_asciinema_advance(struct kmscon_asciinema *player, uint64_t *delay_ns);
void kmscon_asciinema_pause(struct kmscon_asciinema *player);
void kmscon_asciinema_resume(struct kmscon_asciinema *player);
void kmscon_asciinema_stop(struct kmscon_asciinema *player);

#endif /* ASCIINEMA_H */
