/*
 * Tests for the freetype font backend's monospace fontconfig filter.
 *
 * Background: there is no monospace Vazirmatn font, yet on an Arabic locale
 * fontconfig happily offers the proportional Vazirmatn for a "monospace"
 * request (reproduce with `fc-match monospace:lang=ar`). A proportional font
 * used as a glyph fallback breaks column alignment in a cell-based terminal.
 *
 * The backend requests monospace spacing on the pattern AND installs a
 * fontconfig filter (_fc_monospace_font_filter_func). The per-pattern request
 * only re-ranks fonts (proportional ones still sit in the sorted fallback set
 * and can be picked for a glyph the top monospace font lacks); the filter is
 * what removes proportional fonts from the set entirely. That set-wide property
 * is the real guarantee and the one worth testing.
 *
 * Two levels:
 *   1. test_filter_unit          - the filter predicate in isolation.
 *   2. test_monospace_fallback_set - exercise kmscon the way it really runs:
 *      register the backend and kmscon_font_find() it through the
 *      kmscon_font_ops callback table, then check that every font in the
 *      fallback set it built is monospace.
 *
 * The public font API does not expose the fallback set (callers only get
 * metrics and a glyph bitmap), so to inspect it we reach into the backend's
 * private ft_data (font->data). Both src files are #included directly (like
 * test_text.c) to get at that layout and the static filter.
 */

#include <assert.h>
#include <fontconfig/fontconfig.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

#include "../src/font/blocks.c"	       /* block elements, called by font.c */
#include "../src/font/font.c"	       /* real dispatch: register / find / render */
#undef LOG_SUBSYSTEM		       /* font.c and font_freetype.c both define it */
#include "../src/font/font_freetype.c" /* backend ops + ft_data layout */

#define ARABIC_ALEF 0x0627u

/* font.c pulls in shl/module.h; the backend registers with a NULL owner, so
 * module ref-counting is a no-op here. */
void shl_module_ref(struct shl_module *module) {}
void shl_module_unref(struct shl_module *module) {}

static bool spacing_is_monospace(int spacing)
{
	return spacing == FC_CHARCELL || spacing == FC_MONO || spacing == FC_DUAL;
}

/* --- 1. unit test of the filter predicate --- */

static FcPattern *pattern_with_spacing(int spacing)
{
	FcPattern *pat = FcPatternCreate();
	assert(pat);
	assert(FcPatternAddInteger(pat, FC_SPACING, spacing));
	return pat;
}

static void test_filter_unit(void)
{
	FcPattern *pat;

	/* Monospace spacings must be accepted. */
	pat = pattern_with_spacing(FC_CHARCELL);
	assert(_fc_monospace_font_filter_func(pat, NULL) == FcTrue);
	FcPatternDestroy(pat);

	pat = pattern_with_spacing(FC_MONO);
	assert(_fc_monospace_font_filter_func(pat, NULL) == FcTrue);
	FcPatternDestroy(pat);

	pat = pattern_with_spacing(FC_DUAL);
	assert(_fc_monospace_font_filter_func(pat, NULL) == FcTrue);
	FcPatternDestroy(pat);

	/* A proportional font (e.g. Vazirmatn) must be rejected. */
	pat = pattern_with_spacing(FC_PROPORTIONAL);
	assert(_fc_monospace_font_filter_func(pat, NULL) == FcFalse);
	FcPatternDestroy(pat);

	/* Proportional fonts commonly omit FC_SPACING entirely; reject those too
	 * (this is the real-world Vazirmatn shape). */
	pat = FcPatternCreate();
	assert(pat);
	assert(_fc_monospace_font_filter_func(pat, NULL) == FcFalse);
	FcPatternDestroy(pat);
}

/* --- 2. behavioral test through the public kmscon font API --- */

/* Build the pattern kmscon's prepare_font() uses for a "monospace" request. */
static FcPattern *kmscon_monospace_pattern(void)
{
	FcPattern *pat = FcNameParse((const FcChar8 *)KMSCON_FONT_DEFAULT_NAME);
	assert(pat);
	FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_NORMAL);
	FcPatternAddDouble(pat, FC_PIXEL_SIZE, 20.0);
	FcPatternAddInteger(pat, FC_SPACING, FC_CHARCELL);
	FcPatternAddInteger(pat, FC_SPACING, FC_MONO);
	FcPatternAddInteger(pat, FC_SPACING, FC_DUAL);
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	return pat;
}

/*
 * With no kmscon filter active, count the proportional fonts in the "monospace"
 * sorted set for the current locale. If @ch is non-zero, only count fonts that
 * cover @ch. This establishes that the unfiltered set really does contain
 * proportional fonts for the filter to remove -- otherwise the monospace
 * assertions below would hold trivially and guard nothing.
 *
 * Must be called before kmscon_font_find(), i.e. before the filter is installed.
 */
static int unfiltered_proportional_count(uint32_t ch)
{
	FcPattern *pat = kmscon_monospace_pattern();
	FcResult res;
	FcFontSet *fs = FcFontSort(NULL, pat, FcTrue, NULL, &res);
	int count = 0;

	for (int i = 0; fs && i < fs->nfont; i++) {
		int spacing = FC_PROPORTIONAL;

		if (ch) {
			FcCharSet *cs;
			if (FcPatternGetCharSet(fs->fonts[i], FC_CHARSET, 0, &cs) != FcResultMatch)
				continue;
			if (!FcCharSetHasChar(cs, ch))
				continue;
		}
		FcPatternGetInteger(fs->fonts[i], FC_SPACING, 0, &spacing);
		if (!spacing_is_monospace(spacing))
			count++;
	}
	if (fs)
		FcFontSetDestroy(fs);
	FcPatternDestroy(pat);
	return count;
}

static void test_monospace_fallback_set(void)
{
	struct kmscon_font *font = NULL;
	struct ft_data *ftd;
	FcFontSet *set;
	int arabic_fonts = 0;
	int prop_total, prop_arabic;
	int ret;

	/* Use an Arabic locale so the ordering matches the real bug scenario. */
	setlocale(LC_ALL, "ar_AE.UTF-8");

	/* Measure the unfiltered set *before* the filter is installed by find(). */
	prop_total = unfiltered_proportional_count(0);
	prop_arabic = unfiltered_proportional_count(ARABIC_ALEF);

	/* If no fonts are installed, don't go further in the tests */
	if (!prop_total || !prop_arabic)
		return;

	/* Register and look up the backend exactly as kmscon does at runtime;
	 * init() installs the monospace filter as a side effect. */
	ret = kmscon_font_register(&kmscon_font_freetype_ops);
	assert(ret == 0);
	ret = kmscon_font_find(&font, KMSCON_FONT_DEFAULT_NAME, 20, "freetype");
	assert(ret == 0);
	assert(font);
	assert(font->ops == &kmscon_font_freetype_ops); /* freetype really chosen */

	ftd = font->data;
	assert(ftd);
	set = ftd->regular.fc;
	assert(set);

	/*
	 * Core, language-agnostic guarantee: every font kmscon put in the fallback
	 * set carries a proper monospace FC_SPACING, no matter which script it
	 * covers. The proportional Vazirmatn (and every other proportional font) is
	 * therefore absent. Without the filter this set contains proportional fonts
	 * (prop_total of them) and these assertions fail.
	 */
	if (prop_total == 0)
		fprintf(stderr, "note: no proportional fonts installed; the monospace "
				"assertions hold trivially and guard nothing\n");
	for (int i = 0; i < set->nfont; i++) {
		FcChar8 *family = NULL;
		int spacing = -1;

		/* proper, present FC_SPACING, and it must be a monospace value */
		assert(FcPatternGetInteger(set->fonts[i], FC_SPACING, 0, &spacing) ==
		       FcResultMatch);
		assert(spacing_is_monospace(spacing));

		FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &family);
		assert(family);
		assert(strcasestr((const char *)family, "Vazirmatn") == NULL);

		FcCharSet *cs;
		if (FcPatternGetCharSet(set->fonts[i], FC_CHARSET, 0, &cs) == FcResultMatch &&
		    FcCharSetHasChar(cs, ARABIC_ALEF))
			arabic_fonts++;
	}
	fprintf(stderr,
		"kmscon monospace fallback set: %d fonts, %d cover Arabic "
		"(unfiltered had %d proportional, %d covering Arabic)\n",
		set->nfont, arabic_fonts, prop_total, prop_arabic);

	kmscon_font_unref(font);
	kmscon_font_unregister(kmscon_font_freetype_ops.name);
}

/* --- 3. automatic language fallback through the unfiltered set --- */

/* Surrogates and noncharacters aren't real glyphs; a font charset may still
 * list them, so don't pick them as the test code point. */
static bool is_real_scalar(uint32_t ch)
{
	if (ch >= 0xD800u && ch <= 0xDFFFu) /* surrogate */
		return false;
	if (ch >= 0xFDD0u && ch <= 0xFDEFu) /* noncharacter block */
		return false;
	if ((ch & 0xFFFEu) == 0xFFFEu) /* U+xxFFFE / U+xxFFFF */
		return false;
	return true;
}

/*
 * Scan the unfiltered set for a code point that only a filter-rejected font
 * covers, i.e. one the strict monospace set (fc) cannot render. This is exactly
 * the #596 shape (CJK via Noto Sans Mono CJK). Returns 0 if none exists.
 */
static uint32_t find_mono_uncovered_char(struct ft_font *ftf)
{
	for (int i = 0; ftf->fc_unfiltered && i < ftf->fc_unfiltered->nfont; i++) {
		FcPattern *pat = ftf->fc_unfiltered->fonts[i];
		FcChar32 page, next, map[FC_CHARSET_MAP_SIZE];
		FcCharSet *cs;

		/* Only fonts the filter drops can add coverage the strict set lacks. */
		if (_fc_monospace_font_filter_func(pat, NULL) != FcFalse)
			continue;
		if (FcPatternGetCharSet(pat, FC_CHARSET, 0, &cs) != FcResultMatch)
			continue;

		for (page = FcCharSetFirstPage(cs, map, &next); page != FC_CHARSET_DONE;
		     page = FcCharSetNextPage(cs, map, &next)) {
			for (int j = 0; j < FC_CHARSET_MAP_SIZE; j++) {
				for (int b = 0; b < 32 && map[j] >> b; b++) {
					uint32_t ch = page + j * 32 + b;

					if ((map[j] & (1u << b)) && is_real_scalar(ch) &&
					    get_fallback(ch, ftf->fc) < 0)
						return ch;
				}
			}
		}
	}
	return 0;
}

/*
 * The #596 regression: with the default "monospace" request, a code point whose
 * only coverage is a font the monospace filter drops (e.g. CJK via Noto Sans
 * Mono CJK, which omits FC_SPACING) must still be renderable. The backend keeps
 * an unfiltered set (ft_font.fc_unfiltered) for exactly this, consulted only
 * when no monospace font covers the glyph.
 */
static void test_auto_language_fallback(void)
{
	struct kmscon_font *font = NULL;
	struct kmscon_font_attr attr = {0};
	struct ft_data *ftd;
	FcFontSet *set = NULL;
	uint32_t ch;
	int idx, ret;

	ret = kmscon_font_register(&kmscon_font_freetype_ops);
	assert(ret == 0);
	ret = kmscon_font_find(&font, KMSCON_FONT_DEFAULT_NAME, 20, "freetype");
	assert(ret == 0 && font);

	ftd = font->data;
	assert(ftd);

	/* The unfiltered set must be built for the default request and be a
	 * superset of the strict one -- it is what carries the extra coverage. */
	assert(ftd->regular.fc_unfiltered);
	assert(ftd->regular.fc_unfiltered->nfont >= ftd->regular.fc->nfont);

	/* Pick a code point covered by the unfiltered set but not the strict one.
	 * If none exists (minimal font set, or every script has a monospace font)
	 * there is nothing to guard -- skip. */
	ch = find_mono_uncovered_char(&ftd->regular);
	if (!ch) {
		fprintf(stderr, "note: no code point exercises the unfiltered "
				"fallback; skipping\n");
		goto out;
	}

	/* find_fallback must resolve it, and specifically through the unfiltered
	 * set (not the strict one, which was just shown not to cover it). */
	idx = find_fallback(&ftd->regular, ch, &set);
	assert(idx >= 0);
	assert(set == ftd->regular.fc_unfiltered);

	/* The public has_glyph() must agree -- this is what keeps CJK off tofu. */
	assert(kmscon_font_freetype_has_glyph(font, &attr, ch));

	fprintf(stderr,
		"kmscon auto language fallback: U+%04X resolved via the "
		"unfiltered set\n",
		ch);

out:
	kmscon_font_unref(font);
	kmscon_font_unregister(kmscon_font_freetype_ops.name);
}

/* First real scalar code point covered by @fc, or 0. */
static uint32_t first_covered_char(FcFontSet *fc)
{
	for (int i = 0; fc && i < fc->nfont; i++) {
		FcChar32 page, next, map[FC_CHARSET_MAP_SIZE];
		FcCharSet *cs;

		if (FcPatternGetCharSet(fc->fonts[i], FC_CHARSET, 0, &cs) != FcResultMatch)
			continue;
		for (page = FcCharSetFirstPage(cs, map, &next); page != FC_CHARSET_DONE;
		     page = FcCharSetNextPage(cs, map, &next)) {
			for (int j = 0; j < FC_CHARSET_MAP_SIZE; j++)
				for (int b = 0; b < 32 && map[j] >> b; b++)
					if ((map[j] & (1u << b)) &&
					    is_real_scalar(page + j * 32 + b))
						return page + j * 32 + b;
		}
	}
	return 0;
}

/*
 * Regression guard for the fallback-ordering fix (issue #596 follow-up): even
 * when the strict monospace set covers a glyph, fallback must follow
 * fontconfig's own ranking (the unfiltered set) instead of hard-preferring the
 * filtered set. Without the fix, a lesser monospace-tagged font sitting in the
 * filtered set (e.g. Droid Sans Japanese) would be chosen over the
 * locale-preferred Noto Sans Mono CJK that fontconfig ranks first.
 */
static void test_fallback_follows_fontconfig_order(void)
{
	struct kmscon_font *font = NULL;
	struct ft_data *ftd;
	FcFontSet *set = NULL;
	uint32_t ch;
	int idx, ret;

	ret = kmscon_font_register(&kmscon_font_freetype_ops);
	assert(ret == 0);
	ret = kmscon_font_find(&font, KMSCON_FONT_DEFAULT_NAME, 20, "freetype");
	assert(ret == 0 && font);

	ftd = font->data;
	assert(ftd && ftd->regular.fc_unfiltered);

	/* A glyph the strict (filtered) set itself covers -- the case where the old
	 * code would have returned a font from fc. */
	ch = first_covered_char(ftd->regular.fc);
	if (!ch) {
		fprintf(stderr, "note: strict set covers nothing; skipping\n");
		goto out;
	}

	/* The fix: fallback is resolved against the unfiltered, fontconfig-ranked
	 * set, not the filtered one. */
	idx = find_fallback(&ftd->regular, ch, &set);
	assert(idx >= 0);
	assert(set == ftd->regular.fc_unfiltered);

	fprintf(stderr,
		"kmscon fallback order: U+%04X resolved via the unfiltered "
		"(fontconfig-ranked) set\n",
		ch);

out:
	kmscon_font_unref(font);
	kmscon_font_unregister(kmscon_font_freetype_ops.name);
}

/* --- 4. explicitly configured fonts bypass the filter --- */

/* Copy the first FC_FAMILY of @pat, or NULL. Caller frees. */
static char *first_family(FcPattern *pat)
{
	FcChar8 *family = NULL;

	if (FcPatternGetString(pat, FC_FAMILY, 0, &family) != FcResultMatch)
		return NULL;
	return strdup((const char *)family);
}

/* Does any FC_FAMILY value of @pat match @family case-insensitively? */
static bool pattern_has_family(FcPattern *pat, const char *family)
{
	FcChar8 *val = NULL;

	for (int i = 0; FcPatternGetString(pat, FC_FAMILY, i, &val) == FcResultMatch; i++) {
		if (strcasecmp((const char *)val, family) == 0)
			return true;
	}
	return false;
}

/*
 * A user who names a font in the config means it: even a proportional font
 * (which the monospace filter would drop) must be honored as the primary face.
 * Discover an installed proportional family from an unfiltered config, request
 * it by name, and check it really was selected.
 */
static void test_configured_font_bypasses_filter(void)
{
	FcConfig *cfg;
	FcFontSet *sys;
	char *family = NULL;
	struct kmscon_font *font = NULL;
	struct ft_data *ftd;
	int ret;

	/* Find, in a fresh unfiltered config, a font the monospace filter would
	 * reject (proportional, including the common "no FC_SPACING at all" case). */
	cfg = FcInitLoadConfigAndFonts();
	assert(cfg);
	sys = FcConfigGetFonts(cfg, FcSetSystem);
	for (int i = 0; sys && i < sys->nfont && !family; i++) {
		if (_fc_monospace_font_filter_func(sys->fonts[i], NULL) == FcFalse)
			family = first_family(sys->fonts[i]);
	}
	FcConfigDestroy(cfg);

	if (!family) {
		fprintf(stderr, "note: no filter-rejected font installed; skipping\n");
		return;
	}

	ret = kmscon_font_register(&kmscon_font_freetype_ops);
	assert(ret == 0);
	ret = kmscon_font_find(&font, family, 20, "freetype");
	assert(ret == 0 && font);

	ftd = font->data;
	assert(ftd && ftd->regular.fc && ftd->regular.fc->nfont > 0);

	/* The selected primary carries the requested family AND is itself a font
	 * the filter would drop -- so the strict monospace config could never have
	 * returned it. That is the bypass. */
	assert(pattern_has_family(ftd->regular.fc->fonts[0], family));
	assert(_fc_monospace_font_filter_func(ftd->regular.fc->fonts[0], NULL) == FcFalse);

	fprintf(stderr, "kmscon configured font bypass: [%s] honored as primary\n", family);

	free(family);
	kmscon_font_unref(font);
	kmscon_font_unregister(kmscon_font_freetype_ops.name);
}

int main(void)
{
	assert(FcInit());

	test_filter_unit();
	test_monospace_fallback_set();
	test_auto_language_fallback();
	test_fallback_follows_fontconfig_order();
	test_configured_font_bypasses_filter();

	FcFini();
	return 0;
}
