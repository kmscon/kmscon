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

int main(void)
{
	assert(FcInit());

	test_filter_unit();
	test_monospace_fallback_set();

	FcFini();
	return 0;
}
