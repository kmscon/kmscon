#ifndef UTERM_VT_INTERNAL_H
#define UTERM_VT_INTERNAL_H

#include <shl_dlist.h>
#include <uterm_input.h>
#include <uterm_vt.h>

struct uterm_vt_ops {
	void (*destroy)(struct uterm_vt *vt);
	int (*activate)(struct uterm_vt *vt);
	int (*deactivate)(struct uterm_vt *vt);
	void (*input)(struct uterm_vt *vt, struct uterm_input_key_event *ev);
	void (*retry)(struct uterm_vt *vt);
	unsigned int (*get_type)(struct uterm_vt *vt);
	unsigned int (*get_num)(struct uterm_vt *vt);
	void (*bell)(struct uterm_vt *vt);
	int (*restore)(struct uterm_vt *vt);
};

struct uterm_vt {
	struct shl_dlist list;
	struct uterm_vt_master *vtm;
	struct uterm_input *input;

	uterm_vt_cb cb;
	void *data;

	bool active;
	bool hup;

	const struct uterm_vt_ops *ops;
};

struct uterm_vt_master {
	unsigned long ref;
	struct ev_eloop *eloop;

	struct shl_dlist vts;
};

void vt_call_activate(struct uterm_vt *vt, int target);
int vt_call_deactivate(struct uterm_vt *vt, int target, bool force);

struct uterm_vt *uterm_vt_real_new(struct uterm_vt_master *vtm, struct uterm_input *input,
				   const char *vt_name, uterm_vt_cb cb, void *data);
struct uterm_vt *uterm_vt_fake_new(struct uterm_vt_master *vtm, struct uterm_input *input,
				   uterm_vt_cb cb, void *data);

#endif /* UTERM_VT_INTERNAL_H */