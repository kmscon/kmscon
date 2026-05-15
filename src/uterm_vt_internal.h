#ifndef UTERM_VT_INTERNAL_H
#define UTERM_VT_INTERNAL_H

#include "input/input.h"
#include "shl/dlist.h"
#include "shl/eloop.h"
#include "uterm_vt.h"

struct uterm_vt_ops {
	void (*destroy)(struct uterm_vt *vt);
	int (*activate)(struct uterm_vt *vt);
	int (*deactivate)(struct uterm_vt *vt);
	void (*input)(struct uterm_vt *vt, struct uterm_input_key_event *ev);
	void (*retry)(struct uterm_vt *vt);
	unsigned int (*get_num)(struct uterm_vt *vt);
	void (*bell)(struct uterm_vt *vt);
	const char *(*get_name)(struct uterm_vt *vt);
	int (*restore)(struct uterm_vt *vt);
	int (*open_device)(struct uterm_vt *vt, const char *device, int *fd_id);
	void (*close_device)(struct uterm_vt *vt, int fd, int fd_id);
};

struct uterm_vt {
	struct ev_eloop *eloop;
	struct uterm_input *input;

	uterm_vt_cb cb;
	void *data;

	bool active;
	bool hup;

	const struct uterm_vt_ops *ops;
};

void vt_call_activate(struct uterm_vt *vt);
int vt_call_deactivate(struct uterm_vt *vt, bool force);

struct uterm_vt *uterm_vt_real_new(struct ev_eloop *eloop, struct uterm_input *input,
				   const char *vt_name, uterm_vt_cb cb, void *data);
struct uterm_vt *uterm_vt_fake_new(struct ev_eloop *eloop, struct uterm_input *input,
				   uterm_vt_cb cb, void *data);
#ifdef BUILD_ENABLE_LIBSEAT
struct uterm_vt *uterm_vt_libseat_new(struct ev_eloop *eloop, struct uterm_input *input,
				      const char *vt_name, uterm_vt_cb cb, void *data);
#else
static inline struct uterm_vt *uterm_vt_libseat_new(struct ev_eloop *eloop,
						    struct uterm_input *input, const char *vt_name,
						    uterm_vt_cb cb, void *data)
{
	return NULL;
}
#endif /* BUILD_ENABLE_LIBSEAT */

#endif /* UTERM_VT_INTERNAL_H */