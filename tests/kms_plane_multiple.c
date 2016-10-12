/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "igt.h"
#include "drmtest.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

IGT_TEST_DESCRIPTION("Test atomic mode setting with multiple planes ");

#define SIZE 128

typedef struct {
	float red;
	float green;
	float blue;
} color_t;

typedef struct {
	int drm_fd;
	igt_display_t display;
	igt_pipe_crc_t *pipe_crc;
	igt_plane_t *primary;
	igt_plane_t *sprite[IGT_MAX_PLANES-1];
	struct igt_fb primary_fb;
	struct igt_fb sprite_fb[IGT_MAX_PLANES-1];
} data_t;

typedef struct {
	data_t *data;
	igt_crc_t reference_crc;
} test_position_t;

/* Command line parameters. */
struct {
	bool user_seed;
	int seed;
	bool user_logfile;
	char logfile[SIZE];
} opt = {
	.user_seed = false,
	.seed = 1,
	.user_logfile = false,
	.logfile = "kms_plane_multiple.log",
};


static int logwrite(const char *testname)
{
	time_t curr_time;
	FILE *fid;
	char *time_str;

	fid = fopen(opt.logfile, "a");

	if (fid == NULL) {
		igt_debug("Could not open file %s\n", opt.logfile);
		return -1;
	}

	curr_time = time(NULL);

	time_str = ctime(&curr_time);
	time_str[strlen(time_str)-1] = '\0';

	fprintf(fid, "%s: kms_plane_multiple --run-subtest %s --seed %d\n",
		time_str, testname, opt.seed);

	fclose(fid);

	return 0;
}

/*
 * Common code across all tests, acting on data_t
 */
static void test_init(data_t *data, enum pipe pipe)
{
	data->pipe_crc = igt_pipe_crc_new(pipe, INTEL_PIPE_CRC_SOURCE_AUTO);
}

static void test_fini(data_t *data, igt_output_t *output, int nplanes)
{
	igt_plane_set_fb(data->primary, NULL);

	for (int i = 0; i < nplanes; i++)
		igt_plane_set_fb(data->sprite[i], NULL);

	/* reset the constraint on the pipe */
	igt_output_set_pipe(output, PIPE_ANY);

	igt_pipe_crc_free(data->pipe_crc);
}

static void
test_grab_crc(data_t *data, igt_output_t *output, enum pipe pipe,
	      color_t *color, uint64_t tiling, int commit,
	      igt_crc_t *crc /* out */)
{
	struct igt_fb fb;
	drmModeModeInfo *mode;
	igt_plane_t *primary;

	igt_output_set_pipe(output, pipe);

	primary = igt_output_get_plane(output, IGT_PLANE_PRIMARY);

	mode = igt_output_get_mode(output);

	igt_create_color_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    LOCAL_DRM_FORMAT_MOD_NONE,
			    color->red, color->green, color->blue,
			    &fb);

	igt_plane_set_fb(primary, &fb);

	igt_display_commit2(&data->display, commit);

	igt_wait_for_vblank(data->drm_fd, pipe);

	igt_pipe_crc_collect_crc(data->pipe_crc, crc);

	igt_plane_set_fb(primary, NULL);

	igt_display_commit2(&data->display, commit);

	igt_remove_fb(data->drm_fd, &fb);
}

/*
 * Multiple plane position test.
 *   - We start by grabbing a reference CRC of a full blue fb being scanned
 *     out on the primary plane
 *   - Then we scannout number of planes:
 *      * the primary plane uses a blue fb with a black rectangle hole
 *      * planes, on top of the primary plane, with a blue fb that is set-up
 *        to cover the black rectangles of the primary plane fb
 *     The resulting CRC should be identical to the reference CRC
 */

static void
create_fb_for_mode_position(data_t *data, drmModeModeInfo *mode,
			    color_t *color, int *rect_x, int *rect_y,
			    int rect_w, int rect_h, uint64_t tiling,
			    int nplanes)
{
	unsigned int fb_id;
	cairo_t *cr;

	fb_id = igt_create_fb(data->drm_fd,
			      mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      tiling,
			      &data->primary_fb);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(data->drm_fd, &data->primary_fb);
	igt_paint_color(cr, 0, 0, mode->hdisplay, mode->vdisplay,
			color->red, color->green, color->blue);

	for (int i = 0; i < nplanes; i++)
		igt_paint_color(cr, rect_x[i], rect_y[i],
				rect_w, rect_h, 0.0, 0.0, 0.0);

	igt_assert(cairo_status(cr) == 0);
	cairo_destroy(cr);
}


static void
test_planes(data_t *data, enum pipe pipe, color_t *color,
	    uint64_t tiling, int nplanes, igt_output_t *output)
{
	drmModeModeInfo *mode;
	int x[IGT_MAX_PLANES-1];
	int y[IGT_MAX_PLANES-1];
	int plane[IGT_MAX_PLANES-1] = {IGT_PLANE_2,
				       IGT_PLANE_3,
				       IGT_PLANE_4,
				       IGT_PLANE_5,
				       IGT_PLANE_6,
				       IGT_PLANE_7,
				       IGT_PLANE_8,
				       IGT_PLANE_9,
				       IGT_PLANE_CURSOR};

	igt_output_set_pipe(output, pipe);

	mode = igt_output_get_mode(output);

	/* sprite planes with random positions */
	for (int i = 0; i < nplanes; i++) {
		x[i] = rand() % (mode->hdisplay - SIZE);
		y[i] = rand() % (mode->vdisplay - SIZE);

		data->sprite[i] = igt_output_get_plane(output, plane[i]);

		igt_create_color_fb(data->drm_fd,
				    SIZE, SIZE, /* width, height */
				    data->sprite[i]->is_cursor ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888,
				    tiling,
				    color->red, color->green, color->blue,
				    &data->sprite_fb[i]);

		igt_plane_set_position(data->sprite[i], x[i], y[i]);
		igt_plane_set_fb(data->sprite[i], &data->sprite_fb[i]);
	}

	/* primary plane */
	data->primary = igt_output_get_plane(output, IGT_PLANE_PRIMARY);
	create_fb_for_mode_position(data, mode, color, x, y,
				    SIZE, SIZE, tiling, nplanes);
	igt_plane_set_fb(data->primary, &data->primary_fb);
}

static void
test_plane_position_with_output(int n, int iterations, data_t *data,
				enum pipe pipe,
				igt_output_t *output,
				int nplanes, uint64_t tiling,
				int commit)
{
	test_position_t test = { .data = data };
	igt_crc_t crc;
	color_t blue  = { 0.0f, 0.0f, 1.0f };

	igt_info("%d/%d: Testing connector %s using pipe %s with %d planes\n",
		 n, iterations, igt_output_name(output), kmstest_pipe_name(pipe),
		 nplanes);

	test_init(data, pipe);

	test_grab_crc(data, output, pipe, &blue, tiling, commit, &test.reference_crc);

	test_planes(data, pipe, &blue, tiling, nplanes, output);

	igt_display_commit2(&data->display, commit);

	igt_wait_for_vblank(data->drm_fd, pipe);

	igt_pipe_crc_collect_crc(data->pipe_crc, &crc);

	igt_wait_for_vblank(data->drm_fd, pipe);

	igt_assert_crc_equal(&test.reference_crc, &crc);

	test_fini(data, output, nplanes);
}

static void
test_plane_position(data_t *data, enum pipe pipe, int nplanes,
		    uint64_t tiling, int commit)
{
	igt_output_t *output;
	int connected_outs;
	int i;
	int iterations = 12;

	igt_skip_on(pipe >= data->display.n_pipes);
	igt_skip_on(nplanes >= data->display.pipes[pipe].n_planes);

	igt_info("running test for dozen iterations\n");

	if (!opt.user_seed)
		opt.seed = time(NULL);

	srand(opt.seed);
	logwrite(igt_subtest_name());

	for (i = 0; i < iterations; i++) {
		connected_outs = 0;
		for_each_connected_output(&data->display, output) {
			test_plane_position_with_output(i+1, iterations, data,
							pipe, output, nplanes,
							tiling, commit);
			connected_outs++;
		}

		igt_skip_on(connected_outs == 0);
	}

}

static void
run_tests_for_pipe_plane(data_t *data, enum pipe pipe, int nplanes)
{
	igt_subtest_f("legacy-pipe-%s-tiling-none-planes-%d",
		      kmstest_pipe_name(pipe), nplanes)
		test_plane_position(data, pipe, nplanes,
				    LOCAL_DRM_FORMAT_MOD_NONE, COMMIT_LEGACY);

	igt_subtest_f("atomic-pipe-%s-tiling-none-planes-%d",
		      kmstest_pipe_name(pipe), nplanes)
		test_plane_position(data, pipe, nplanes,
				    LOCAL_I915_FORMAT_MOD_X_TILED, COMMIT_ATOMIC);

	igt_subtest_f("legacy-pipe-%s-tiling-x-planes-%d",
		      kmstest_pipe_name(pipe), nplanes)
		test_plane_position(data, pipe, nplanes,
				    LOCAL_I915_FORMAT_MOD_X_TILED, COMMIT_LEGACY);

	igt_subtest_f("atomic-pipe-%s-tiling-x-planes-%d",
		      kmstest_pipe_name(pipe), nplanes)
		test_plane_position(data, pipe, nplanes,
				    LOCAL_I915_FORMAT_MOD_X_TILED, COMMIT_ATOMIC);

	igt_subtest_f("legacy-pipe-%s-tiling-y-planes-%d",
		      kmstest_pipe_name(pipe), nplanes)
		test_plane_position(data, pipe, nplanes,
				    LOCAL_I915_FORMAT_MOD_Y_TILED, COMMIT_LEGACY);

	igt_subtest_f("atomic-pipe-%s-tiling-y-planes-%d",
		      kmstest_pipe_name(pipe), nplanes)
		test_plane_position(data, pipe, nplanes,
				    LOCAL_I915_FORMAT_MOD_Y_TILED, COMMIT_ATOMIC);

	igt_subtest_f("legacy-pipe-%s-tiling-yf-planes-%d",
		      kmstest_pipe_name(pipe), nplanes)
		test_plane_position(data, pipe, nplanes,
				    LOCAL_I915_FORMAT_MOD_Yf_TILED, COMMIT_LEGACY);

	igt_subtest_f("atomic-pipe-%s-tiling-yf-planes-%d",
		      kmstest_pipe_name(pipe), nplanes)
		test_plane_position(data, pipe, nplanes,
				    LOCAL_I915_FORMAT_MOD_Yf_TILED, COMMIT_ATOMIC);
}

static void
run_tests_for_pipe(data_t *data, enum pipe pipe)
{
	for (int nplanes = 1; nplanes < IGT_MAX_PLANES - 1; nplanes++)
		run_tests_for_pipe_plane(data, pipe, nplanes);
}

static data_t data;

static int opt_handler(int option, int option_index, void *input)
{
	switch (option) {
	case 's':
		opt.user_seed = true;
		opt.seed = strtol(optarg, NULL, 0);
		break;
	case 'l':
		opt.user_logfile = true;
		strcpy(opt.logfile, optarg);
		break;
	default:
		igt_assert(false);
	}

	return 0;
}

const char *help_str =
	"  --seed       Seed for random number generator\n"
	"  --logfile    Logfile to store seeds for random number generator, default 'kms_plane_multiple.log'\n";

int main(int argc, char *argv[])
{
	time_t t = time(NULL);
	struct tm tm = *localtime(&t);

	struct option long_options[] = {
		{ "seed",    required_argument, NULL, 's'},
		{ "logfile", required_argument, NULL, 'l'},
		{ 0, 0, 0, 0 }
	};

	igt_subtest_init_parse_opts(&argc, argv, "", long_options, help_str,
				    opt_handler, NULL);

	if (!opt.user_logfile)
		sprintf(opt.logfile, "kms_plane_multiple-%4d-%2d-%2d.log",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

	igt_skip_on_simulation();

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL);

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc();
		igt_display_init(&data.display, data.drm_fd);
	}

	for (int pipe = 0; pipe < I915_MAX_PIPES; pipe++)
		run_tests_for_pipe(&data, pipe);

	igt_fixture {
		igt_display_fini(&data.display);
	}

	igt_exit();
}
