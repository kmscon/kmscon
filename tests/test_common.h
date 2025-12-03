/*
 * Kmscon - Test Helper
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Test Helper
 * This header includes all kinds of helpers for testing. It tries to include
 * everything required and provides simple macros to avoid duplicating code in
 * each test. We try to keep tests as small as possible and move everything that
 * might be common here.
 *
 * We avoid sticking to our usual coding conventions (including headers in
 * source files, etc. ..) and instead make this the most convenient we can.
 */

#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <check.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>

/* lower address-space is protected from user-allocation, so this is invalid */
#define TEST_INVALID_PTR ((void *)0x10)

#define UNUSED(x) (void)(x)

#define TEST_DEFINE_CASE(_name)                                                                    \
	static TCase *test_create_case_##_name(void)                                               \
	{                                                                                          \
		TCase *tc;                                                                         \
                                                                                                   \
		tc = tcase_create(#_name);

#define TEST(_name) tcase_add_test(tc, _name);

#define TEST_END_CASE                                                                              \
	return tc;                                                                                 \
	}

#define TEST_END NULL

#define TEST_CASE(_name) test_create_case_##_name

static inline Suite *test_create_suite(const char *name, ...)
{
	Suite *s;
	va_list list;
	TCase *(*fn)(void);

	s = suite_create(name);

	va_start(list, name);
	while ((fn = va_arg(list, TCase * (*)(void))))
		suite_add_tcase(s, fn());
	va_end(list);

	return s;
}

#define TEST_SUITE(_name, ...) test_create_suite((#_name), ##__VA_ARGS__)

static inline int test_run_suite(Suite *s)
{
	int ret;
	SRunner *sr;

	sr = srunner_create(s);
	srunner_run_all(sr, CK_ENV);
	ret = srunner_ntests_failed(sr);
	srunner_free(sr);

	return ret;
}

#define TEST_DEFINE(_suite)                                                                        \
	int main(int argc, char **argv)                                                            \
	{                                                                                          \
		return test_run_suite(_suite);                                                     \
	}

#ifndef ck_assert_mem_eq
#include <string.h>
#define ck_assert_mem_eq(_x, _y, _len) ck_assert(memcmp((_x), (_y), (_len)) == 0)
#define ck_assert_mem_ne(_x, _y, _len) ck_assert(memcmp((_x), (_y), (_len)) != 0)
#define ck_assert_mem_lt(_x, _y, _len) ck_assert(memcmp((_x), (_y), (_len)) < 0)
#define ck_assert_mem_le(_x, _y, _len) ck_assert(memcmp((_x), (_y), (_len)) <= 0)
#define ck_assert_mem_gt(_x, _y, _len) ck_assert(memcmp((_x), (_y), (_len)) > 0)
#define ck_assert_mem_ge(_x, _y, _len) ck_assert(memcmp((_x), (_y), (_len)) >= 0)
#endif

#endif /* TEST_COMMON_H */
