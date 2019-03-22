/*****************************************************************************\
 *  slurm_opt.c - salloc/sbatch/srun option processing functions
 *****************************************************************************
 *  Copyright (C) 2019 SchedMD LLC.
 *  Written by Tim Wickberg <tim@schedmd.com>
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <getopt.h>
#include <sys/param.h>

#include "src/common/cpu_frequency.h"
#include "src/common/log.h"
#include "src/common/optz.h"
#include "src/common/parse_time.h"
#include "src/common/proc_args.h"
#include "src/common/slurm_acct_gather_profile.h"
#include "src/common/slurm_resource_info.h"
#include "src/common/util-net.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/common/slurm_opt.h"

/*
 * This is ugly. But... less ugly than dozens of identical functions handling
 * variables that are just strings pushed and pulled out of the associated
 * structures.
 *
 * This takes one argument: the desired field in slurm_opt_t.
 * The function name will be automatically generated as arg_set_##field.
 */
#define COMMON_STRING_OPTION(field)	\
COMMON_STRING_OPTION_SET(field)		\
COMMON_STRING_OPTION_GET(field)		\
COMMON_STRING_OPTION_RESET(field)
#define COMMON_STRING_OPTION_GET_AND_RESET(field)	\
COMMON_STRING_OPTION_GET(field)				\
COMMON_STRING_OPTION_RESET(field)
#define COMMON_STRING_OPTION_SET(field)				\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
__attribute__((nonnull (1)));					\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
{								\
	xfree(opt->field);					\
	opt->field = xstrdup(arg);				\
								\
	return SLURM_SUCCESS;					\
}
#define COMMON_STRING_OPTION_GET(field)				\
static char *arg_get_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	return xstrdup(opt->field);				\
}
#define COMMON_STRING_OPTION_RESET(field)			\
static void arg_reset_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static void arg_reset_##field(slurm_opt_t *opt)			\
{								\
	xfree(opt->field);					\
}

#define COMMON_SBATCH_STRING_OPTION(field)	\
COMMON_SBATCH_STRING_OPTION_SET(field)		\
COMMON_SBATCH_STRING_OPTION_GET(field)		\
COMMON_SBATCH_STRING_OPTION_RESET(field)
#define COMMON_SBATCH_STRING_OPTION_SET(field)			\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
__attribute__((nonnull (1)));					\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
{								\
	if (!opt->sbatch_opt)					\
		return SLURM_ERROR;				\
								\
	xfree(opt->sbatch_opt->field);				\
	opt->sbatch_opt->field = xstrdup(arg);			\
								\
	return SLURM_SUCCESS;					\
}
#define COMMON_SBATCH_STRING_OPTION_GET(field)			\
static char *arg_get_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	if (!opt->sbatch_opt)					\
		return xstrdup("invalid-context");		\
	return xstrdup(opt->sbatch_opt->field);			\
}
#define COMMON_SBATCH_STRING_OPTION_RESET(field)		\
static void arg_reset_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static void arg_reset_##field(slurm_opt_t *opt)			\
{								\
	if (opt->sbatch_opt)					\
		xfree(opt->sbatch_opt->field);			\
}

#define COMMON_OPTION_RESET(field, value)			\
static void arg_reset_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static void arg_reset_##field(slurm_opt_t *opt)			\
{								\
	opt->field = value;					\
}

#define COMMON_BOOL_OPTION(field, option)			\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
__attribute__((nonnull (1)));					\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
{								\
	opt->field = true;					\
								\
	return SLURM_SUCCESS;					\
}								\
static char *arg_get_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	return xstrdup(opt->field ? "set" : "unset");		\
}								\
COMMON_OPTION_RESET(field, false)

#define COMMON_SRUN_BOOL_OPTION(field)				\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
__attribute__((nonnull (1)));					\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
{								\
	if (!opt->srun_opt)					\
		return SLURM_ERROR;				\
								\
	opt->srun_opt->field = true;				\
								\
	return SLURM_SUCCESS;					\
}								\
static char *arg_get_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	if (!opt->srun_opt)					\
		return xstrdup("invalid-context");		\
								\
	return xstrdup(opt->srun_opt->field ? "set" : "unset");	\
}								\
static void arg_reset_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static void arg_reset_##field(slurm_opt_t *opt)			\
{								\
	if (opt->srun_opt)					\
		opt->srun_opt->field = false;			\
}

#define COMMON_INT_OPTION(field, option)			\
COMMON_INT_OPTION_SET(field, option)				\
COMMON_INT_OPTION_GET(field)					\
COMMON_OPTION_RESET(field, 0)
#define COMMON_INT_OPTION_GET_AND_RESET(field)			\
COMMON_INT_OPTION_GET(field)					\
COMMON_OPTION_RESET(field, 0)
#define COMMON_INT_OPTION_SET(field, option)			\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
__attribute__((nonnull (1)));					\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
{								\
	opt->field = parse_int(option, arg, true);		\
								\
	return SLURM_SUCCESS;					\
}
#define COMMON_INT_OPTION_GET(field)				\
static char *arg_get_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	return xstrdup_printf("%d", opt->field);		\
}

#define COMMON_MBYTES_OPTION(field, option)			\
COMMON_MBYTES_OPTION_SET(field, option)				\
COMMON_MBYTES_OPTION_GET(field)					\
COMMON_OPTION_RESET(field, NO_VAL64)
#define COMMON_MBYTES_OPTION_SET(field, option)			\
static int arg_set_##field(slurm_opt_t *opt, const char *arg)	\
{								\
	if ((opt->field = str_to_mbytes2(arg)) == NO_VAL64) {	\
		error("Invalid ##field specification");		\
		exit(-1);					\
	}							\
								\
        return SLURM_SUCCESS;					\
}
#define COMMON_MBYTES_OPTION_GET_AND_RESET(field)		\
COMMON_MBYTES_OPTION_GET(field)					\
COMMON_OPTION_RESET(field, NO_VAL64)
#define COMMON_MBYTES_OPTION_GET(field)				\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	return mbytes2_to_str(opt->field);			\
}

#define COMMON_TIME_DURATION_OPTION_GET_AND_RESET(field)	\
static char *arg_get_##field(slurm_opt_t *opt)			\
__attribute__((nonnull));					\
static char *arg_get_##field(slurm_opt_t *opt)			\
{								\
	char time_str[32];					\
	secs2time_str(opt->field, time_str, sizeof(time_str));	\
	return xstrdup(time_str);				\
}								\
COMMON_OPTION_RESET(field, NO_VAL)

typedef struct {
	/*
	 * DO NOT ALTER THESE FIRST FOUR ARGUMENTS
	 * They must match 'struct option', so that some
	 * casting abuse is nice and trivial.
	 */
	const char *name;	/* Long option name. */
	int has_arg;		/* no_argument, required_argument,
				 * or optional_argument */
	int *flag;		/* Always NULL in our usage. */
	int val;		/* Single character, or LONG_OPT_* */
	/*
	 * Add new members below here:
	 */
	bool set;		/* Has the option been set */
	bool set_by_env;	/* Has the option been set by env var */
	bool reset_each_pass;	/* Reset on all HetJob passes or only first */
	bool sbatch_early_pass;	/* For sbatch - run in the early pass. */
				/* For salloc/srun - this is ignored, and will
				 * run alongside all other options. */
	/*
	 * If set_func is set, it will be used, and the command
	 * specific versions must not be set.
	 * Otherwise, command specific versions will be used.
	 */
	int (*set_func)(slurm_opt_t *, const char *);
	int (*set_func_salloc)(slurm_opt_t *, const char *);
	int (*set_func_sbatch)(slurm_opt_t *, const char *);
	int (*set_func_srun)(slurm_opt_t *, const char *);
	/* Return must be xfree()'d */
	char *(*get_func)(slurm_opt_t *);
	void (*reset_func)(slurm_opt_t *);
} slurm_cli_opt_t;

/*
 * Function names should be directly correlated with the slurm_opt_t field
 * they manipulate. But the slurm_cli_opt_t name should always match that
 * of the long-form name for the argument itself.
 *
 * These should be alphabetized by the slurm_cli_opt_t name.
 */

COMMON_STRING_OPTION(account);
static slurm_cli_opt_t slurm_opt_account = {
	.name = "account",
	.has_arg = required_argument,
	.val = 'A',
	.set_func = arg_set_account,
	.get_func = arg_get_account,
	.reset_func = arg_reset_account,
};

static int arg_set_acctg_freq(slurm_opt_t *opt, const char *arg)
{
	xfree(opt->acctg_freq);
	opt->acctg_freq = xstrdup(arg);
	if (validate_acctg_freq(opt->acctg_freq))
		exit(-1);

	return SLURM_SUCCESS;
}
COMMON_STRING_OPTION_GET_AND_RESET(acctg_freq);
static slurm_cli_opt_t slurm_opt_acctg_freq = {
	.name = "acctg-freq",
	.has_arg = required_argument,
	.val = LONG_OPT_ACCTG_FREQ,
	.set_func = arg_set_acctg_freq,
	.get_func = arg_get_acctg_freq,
	.reset_func = arg_reset_acctg_freq,
};

static int arg_set_begin(slurm_opt_t *opt, const char *arg)
{
	if (!(opt->begin = parse_time(arg, 0))) {
		error("Invalid --begin specification");
		exit(-1);
	}

	return SLURM_SUCCESS;
}
static char *arg_get_begin(slurm_opt_t *opt)
{
	char time_str[32];
	slurm_make_time_str(&opt->begin, time_str, sizeof(time_str));
	return xstrdup(time_str);
}
COMMON_OPTION_RESET(begin, 0);
static slurm_cli_opt_t slurm_opt_begin = {
	.name = "begin",
	.has_arg = required_argument,
	.val = 'b',
	.set_func = arg_set_begin,
	.get_func = arg_get_begin,
	.reset_func = arg_reset_begin,
};

/* Also see --no-bell below */
static int arg_set_bell(slurm_opt_t *opt, const char *arg)
{
	if (opt->salloc_opt)
		opt->salloc_opt->bell = BELL_ALWAYS;

	return SLURM_SUCCESS;
}
static char *arg_get_bell(slurm_opt_t *opt)
{
	if (!opt->salloc_opt)
		return xstrdup("invalid-context");

	if (opt->salloc_opt->bell == BELL_ALWAYS)
		return xstrdup("bell-always");
	else if (opt->salloc_opt->bell == BELL_AFTER_DELAY)
		return xstrdup("bell-after-delay");
	else if (opt->salloc_opt->bell == BELL_NEVER)
		return xstrdup("bell-never");
	return NULL;
}
static void arg_reset_bell(slurm_opt_t *opt)
{
	if (opt->salloc_opt)
		opt->salloc_opt->bell = BELL_AFTER_DELAY;
}
static slurm_cli_opt_t slurm_opt_bell = {
	.name = "bell",
	.has_arg = no_argument,
	.val = LONG_OPT_BELL,
	.set_func_salloc = arg_set_bell,
	.get_func = arg_get_bell,
	.reset_func = arg_reset_bell,
};

COMMON_STRING_OPTION(burst_buffer);
static slurm_cli_opt_t slurm_opt_bb = {
	.name = "bb",
	.has_arg = required_argument,
	.val = LONG_OPT_BURST_BUFFER_SPEC,
	.set_func = arg_set_burst_buffer,
	.get_func = arg_get_burst_buffer,
	.reset_func = arg_reset_burst_buffer,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(c_constraint);
static slurm_cli_opt_t slurm_opt_c_constraint = {
	.name = "cluster-constraint",
	.has_arg = required_argument,
	.val = LONG_OPT_CLUSTER_CONSTRAINT,
	.set_func = arg_set_c_constraint,
	.get_func = arg_get_c_constraint,
	.reset_func = arg_reset_c_constraint,
};

static int arg_set_chdir(slurm_opt_t *opt, const char *arg)
{
	if (is_full_path(arg))
		opt->chdir = xstrdup(arg);
	else
		opt->chdir = make_full_path(arg);

	return SLURM_SUCCESS;
}
COMMON_STRING_OPTION_GET(chdir);
static void arg_reset_chdir(slurm_opt_t *opt)
{
	char buf[MAXPATHLEN + 1];
	xfree(opt->chdir);
	if (opt->salloc_opt)
		return;

	if (!(getcwd(buf, MAXPATHLEN))) {
		error("getcwd failed: %m");
		exit(-1);
	}
	opt->chdir = xstrdup(buf);
}
static slurm_cli_opt_t slurm_opt_chdir = {
	.name = "chdir",
	.has_arg = required_argument,
	.val = 'D',
	.set_func = arg_set_chdir,
	.get_func = arg_get_chdir,
	.reset_func = arg_reset_chdir,
};

/* --clusters and --cluster are equivalent */
COMMON_STRING_OPTION(clusters);
static slurm_cli_opt_t slurm_opt_clusters = {
	.name = "clusters",
	.has_arg = required_argument,
	.val = 'M',
	.set_func = arg_set_clusters,
	.get_func = arg_get_clusters,
	.reset_func = arg_reset_clusters,
};
static slurm_cli_opt_t slurm_opt_cluster = {
	.name = "cluster",
	.has_arg = required_argument,
	.val = LONG_OPT_CLUSTER,
	.set_func = arg_set_clusters,
	.get_func = arg_get_clusters,
	.reset_func = arg_reset_clusters,
};

COMMON_STRING_OPTION(comment);
static slurm_cli_opt_t slurm_opt_comment = {
	.name = "comment",
	.has_arg = required_argument,
	.val = LONG_OPT_COMMENT,
	.set_func = arg_set_comment,
	.get_func = arg_get_comment,
	.reset_func = arg_reset_comment,
};

COMMON_STRING_OPTION(constraint);
static slurm_cli_opt_t slurm_opt_constraint = {
	.name = "constraint",
	.has_arg = required_argument,
	.val = 'C',
	.set_func = arg_set_constraint,
	.get_func = arg_get_constraint,
	.reset_func = arg_reset_constraint,
	.reset_each_pass = true,
};

COMMON_BOOL_OPTION(contiguous, "contiguous");
static slurm_cli_opt_t slurm_opt_contiguous = {
	.name = "contiguous",
	.has_arg = no_argument,
	.val = LONG_OPT_CONTIGUOUS,
	.set_func = arg_set_contiguous,
	.get_func = arg_get_contiguous,
	.reset_func = arg_reset_contiguous,
	.reset_each_pass = true,
};

static int arg_set_core_spec(slurm_opt_t *opt, const char *arg)
{
	if (opt->srun_opt)
		opt->srun_opt->core_spec_set = true;

	opt->core_spec = parse_int("--core-spec", arg, false);

	return SLURM_SUCCESS;
}
static char *arg_get_core_spec(slurm_opt_t *opt)
{
	if ((opt->core_spec == NO_VAL16) ||
	    (opt->core_spec & CORE_SPEC_THREAD))
		return xstrdup("unset");
	return xstrdup_printf("%d", opt->core_spec);
}
static void arg_reset_core_spec(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->core_spec_set = false;

	opt->core_spec = NO_VAL16;
}
static slurm_cli_opt_t slurm_opt_core_spec = {
	.name = "core-spec",
	.has_arg = required_argument,
	.val = 'S',
	.set_func = arg_set_core_spec,
	.get_func = arg_get_core_spec,
	.reset_func = arg_reset_core_spec,
	.reset_each_pass = true,
};

static int arg_set_cpu_freq(slurm_opt_t *opt, const char *arg)
{
	if (cpu_freq_verify_cmdline(arg, &opt->cpu_freq_min,
				    &opt->cpu_freq_max, &opt->cpu_freq_gov)) {
		error("Invalid --cpu-freq argument");
		exit(-1);
	}

	return SLURM_SUCCESS;
}
static char *arg_get_cpu_freq(slurm_opt_t *opt)
{
	return cpu_freq_to_cmdline(opt->cpu_freq_min,
				   opt->cpu_freq_max,
				   opt->cpu_freq_gov);
}
static void arg_reset_cpu_freq(slurm_opt_t *opt)
{
	opt->cpu_freq_min = NO_VAL;
	opt->cpu_freq_max = NO_VAL;
	opt->cpu_freq_gov = NO_VAL;
}
static slurm_cli_opt_t slurm_opt_cpu_freq = {
	.name = "cpu-freq",
	.has_arg = required_argument,
	.val = LONG_OPT_CPU_FREQ,
	.set_func = arg_set_cpu_freq,
	.get_func = arg_get_cpu_freq,
	.reset_func = arg_reset_cpu_freq,
	.reset_each_pass = true,
};

COMMON_INT_OPTION(cpus_per_gpu, "--cpus-per-gpu");
static slurm_cli_opt_t slurm_opt_cpus_per_gpu = {
	.name = "cpus-per-gpu",
	.has_arg = required_argument,
	.val = LONG_OPT_CPUS_PER_GPU,
	.set_func = arg_set_cpus_per_gpu,
	.get_func = arg_get_cpus_per_gpu,
	.reset_func = arg_reset_cpus_per_gpu,
};

static int arg_set_deadline(slurm_opt_t *opt, const char *arg)
{
	if (!(opt->deadline = parse_time(arg, 0))) {
		error("Invalid --deadline specification");
		exit(-1);
	}

	return SLURM_SUCCESS;
}
static char *arg_get_deadline(slurm_opt_t *opt)
{
	char time_str[32];
	slurm_make_time_str(&opt->deadline, time_str, sizeof(time_str));
	return xstrdup(time_str);
}
COMMON_OPTION_RESET(deadline, 0);
static slurm_cli_opt_t slurm_opt_deadline = {
	.name = "deadline",
	.has_arg = required_argument,
	.val = LONG_OPT_DEADLINE,
	.set_func = arg_set_deadline,
	.get_func = arg_get_deadline,
	.reset_func = arg_reset_deadline,
};

static int arg_set_delay_boot(slurm_opt_t *opt, const char *arg)
{
	if ((opt->delay_boot = time_str2secs(arg)) == NO_VAL) {
		error("Invalid --delay-boot specification");
		exit(-1);
	}

	return SLURM_SUCCESS;
}
COMMON_TIME_DURATION_OPTION_GET_AND_RESET(delay_boot);
static slurm_cli_opt_t slurm_opt_delay_boot = {
	.name = "delay-boot",
	.has_arg = required_argument,
	.val = LONG_OPT_DELAY_BOOT,
	.set_func = arg_set_delay_boot,
	.get_func = arg_get_delay_boot,
	.reset_func = arg_reset_delay_boot,
};

COMMON_STRING_OPTION(dependency);
static slurm_cli_opt_t slurm_opt_dependency = {
	.name = "dependency",
	.has_arg = required_argument,
	.val = 'd',
	.set_func = arg_set_dependency,
	.get_func = arg_get_dependency,
	.reset_func = arg_reset_dependency,
};

static int arg_set_distribution(slurm_opt_t *opt, const char *arg)
{
	opt->distribution = verify_dist_type(arg, &opt->plane_size);
	if (opt->distribution == SLURM_DIST_UNKNOWN) {
		error("Invalid --distribution specification");
		exit(-1);
	}

	return SLURM_SUCCESS;
}
static char *arg_get_distribution(slurm_opt_t *opt)
{
	char *dist = xstrdup(format_task_dist_states(opt->distribution));
	if (opt->distribution == SLURM_DIST_PLANE)
		xstrfmtcat(dist, "=%u", opt->plane_size);
	return dist;
}
static void arg_reset_distribution(slurm_opt_t *opt)
{
	opt->distribution = SLURM_DIST_UNKNOWN;
	opt->plane_size = NO_VAL;
}
static slurm_cli_opt_t slurm_opt_distribution = {
	.name = "distribution",
	.has_arg = required_argument,
	.val = 'm',
	.set_func = arg_set_distribution,
	.get_func = arg_get_distribution,
	.reset_func = arg_reset_distribution,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(exclude);
static slurm_cli_opt_t slurm_opt_exclude = {
	.name = "exclude",
	.has_arg = required_argument,
	.val = 'x',
	.set_func = arg_set_exclude,
	.get_func = arg_get_exclude,
	.reset_func = arg_reset_exclude,
};

static int arg_set_exclusive(slurm_opt_t *opt, const char *arg)
{
	if (!arg || !xstrcasecmp(arg, "exclusive")) {
		if (opt->srun_opt)
			opt->srun_opt->exclusive = true;
		opt->shared = JOB_SHARED_NONE;
	} else if (!xstrcasecmp(arg, "oversubscribe")) {
		opt->shared = JOB_SHARED_OK;
	} else if (!xstrcasecmp(arg, "user")) {
		opt->shared = JOB_SHARED_USER;
	} else if (!xstrcasecmp(arg, "mcs")) {
		opt->shared = JOB_SHARED_MCS;
	} else {
		error("Invalid --exclusive specification");
		exit(-1);
	}

	return SLURM_SUCCESS;
}
static char *arg_get_exclusive(slurm_opt_t *opt)
{
	if (opt->shared == JOB_SHARED_NONE)
		return xstrdup("exclusive");
	if (opt->shared == JOB_SHARED_OK)
		return xstrdup("oversubscribe");
	if (opt->shared == JOB_SHARED_USER)
		return xstrdup("user");
	if (opt->shared == JOB_SHARED_MCS)
		return xstrdup("mcs");
	if (opt->shared == NO_VAL16)
		return xstrdup("unset");
	return NULL;
}
/* warning: shared with --oversubscribe below */
static void arg_reset_shared(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->exclusive = false;
	opt->shared = NO_VAL16;
}
static slurm_cli_opt_t slurm_opt_exclusive = {
	.name = "exclusive",
	.has_arg = optional_argument,
	.val = LONG_OPT_EXCLUSIVE,
	.set_func = arg_set_exclusive,
	.get_func = arg_get_exclusive,
	.reset_func = arg_reset_shared,
	.reset_each_pass = true,
};

static int arg_set_extra_node_info(slurm_opt_t *opt, const char *arg)
{
	cpu_bind_type_t *cpu_bind_type = NULL;

	if (opt->srun_opt)
		cpu_bind_type = &opt->srun_opt->cpu_bind_type;
	opt->extra_set = verify_socket_core_thread_count(arg,
							 &opt->sockets_per_node,
							 &opt->cores_per_socket,
							 &opt->threads_per_core,
							 cpu_bind_type);

	if (!opt->extra_set) {
		error("Invalid --extra-node-info specification");
		exit(-1);
	}

	opt->threads_per_core_set = true;
	if (opt->srun_opt)
		opt->srun_opt->cpu_bind_type_set = true;

	return SLURM_SUCCESS;
}
static char *arg_get_extra_node_info(slurm_opt_t *opt)
{
	char *tmp = NULL;
	if (opt->sockets_per_node != NO_VAL)
		xstrfmtcat(tmp, "%d", opt->sockets_per_node);
	if (opt->cores_per_socket != NO_VAL)
		xstrfmtcat(tmp, ":%d", opt->cores_per_socket);
	if (opt->threads_per_core != NO_VAL)
		xstrfmtcat(tmp, ":%d", opt->threads_per_core);

	if (!tmp)
		return xstrdup("unset");
	return tmp;
}
static void arg_reset_extra_node_info(slurm_opt_t *opt)
{
	opt->extra_set = false;
	opt->sockets_per_node = NO_VAL;
	opt->cores_per_socket = NO_VAL;
	opt->threads_per_core = NO_VAL;
	opt->threads_per_core_set = false;
	if (opt->srun_opt)
                opt->srun_opt->cpu_bind_type_set = false;
}
static slurm_cli_opt_t slurm_opt_extra_node_info = {
	.name = "extra-node-info",
	.has_arg = required_argument,
	.val = 'B',
	.set_func = arg_set_extra_node_info,
	.get_func = arg_get_extra_node_info,
	.reset_func = arg_reset_extra_node_info,
	.reset_each_pass = true,
};

static int arg_set_get_user_env(slurm_opt_t *opt, const char *arg)
{
	char *end_ptr;

	if (!arg) {
		opt->get_user_env_time = 0;
		return SLURM_SUCCESS;
	}

	opt->get_user_env_time = strtol(arg, &end_ptr, 10);

	if (!end_ptr || (end_ptr[0] == '\0'))
		return SLURM_SUCCESS;

	if ((end_ptr[0] == 's') || (end_ptr[0] == 'S'))
		opt->get_user_env_mode = 1;
	else if ((end_ptr[0] == 'l') || (end_ptr[0] == 'L'))
		opt->get_user_env_mode = 2;
	else {
		error("Invalid --get-user-env specification");
		exit(-1);
	}

	return SLURM_SUCCESS;
}
static char *arg_get_get_user_env(slurm_opt_t *opt)
{
	if (opt->get_user_env_mode == 1)
		return xstrdup_printf("%dS", opt->get_user_env_time);
	else if (opt->get_user_env_mode == 2)
		return xstrdup_printf("%dL", opt->get_user_env_time);
	else if (opt->get_user_env_time != -1)
		return xstrdup_printf("%d", opt->get_user_env_time);
	return NULL;
}
static void arg_reset_get_user_env(slurm_opt_t *opt)
{
	opt->get_user_env_mode = -1;
	opt->get_user_env_time = -1;
}
static slurm_cli_opt_t slurm_opt_get_user_env = {
	.name = "get-user-env",
	.has_arg = optional_argument,
	.val = LONG_OPT_GET_USER_ENV,
	.set_func_salloc = arg_set_get_user_env,
	.set_func_sbatch = arg_set_get_user_env,
	.get_func = arg_get_get_user_env,
	.reset_func = arg_reset_get_user_env,
};

COMMON_STRING_OPTION(gpu_bind);
static slurm_cli_opt_t slurm_opt_gpu_bind = {
	.name = "gpu-bind",
	.has_arg = required_argument,
	.val = LONG_OPT_GPU_BIND,
	.set_func = arg_set_gpu_bind,
	.get_func = arg_get_gpu_bind,
	.reset_func = arg_reset_gpu_bind,
};

COMMON_STRING_OPTION(gpu_freq);
static slurm_cli_opt_t slurm_opt_gpu_freq = {
	.name = "gpu-freq",
	.has_arg = required_argument,
	.val = LONG_OPT_GPU_FREQ,
	.set_func = arg_set_gpu_freq,
	.get_func = arg_get_gpu_freq,
	.reset_func = arg_reset_gpu_freq,
};

COMMON_STRING_OPTION(gpus);
static slurm_cli_opt_t slurm_opt_gpus = {
	.name = "gpus",
	.has_arg = required_argument,
	.val = 'G',
	.set_func = arg_set_gpus,
	.get_func = arg_get_gpus,
	.reset_func = arg_reset_gpus,
};

COMMON_STRING_OPTION(gpus_per_node);
static slurm_cli_opt_t slurm_opt_gpus_per_node = {
	.name = "gpus-per-node",
	.has_arg = required_argument,
	.val = LONG_OPT_GPUS_PER_NODE,
	.set_func = arg_set_gpus_per_node,
	.get_func = arg_get_gpus_per_node,
	.reset_func = arg_reset_gpus_per_node,
};

COMMON_STRING_OPTION(gpus_per_socket);
static slurm_cli_opt_t slurm_opt_gpus_per_socket = {
	.name = "gpus-per-socket",
	.has_arg = required_argument,
	.val = LONG_OPT_GPUS_PER_SOCKET,
	.set_func = arg_set_gpus_per_socket,
	.get_func = arg_get_gpus_per_socket,
	.reset_func = arg_reset_gpus_per_socket,
};

COMMON_STRING_OPTION(gpus_per_task);
static slurm_cli_opt_t slurm_opt_gpus_per_task = {
	.name = "gpus-per-task",
	.has_arg = required_argument,
	.val = LONG_OPT_GPUS_PER_TASK,
	.set_func = arg_set_gpus_per_task,
	.get_func = arg_get_gpus_per_task,
	.reset_func = arg_reset_gpus_per_task,
};

static int arg_set_gres(slurm_opt_t *opt, const char *arg)
{
	if (!xstrcasecmp(arg, "help") || !xstrcasecmp(arg, "list")) {
		print_gres_help();
		exit(0);
	}

	xfree(opt->gres);
	opt->gres = xstrdup(arg);

	return SLURM_SUCCESS;
}
COMMON_STRING_OPTION_GET_AND_RESET(gres);
static slurm_cli_opt_t slurm_opt_gres = {
	.name = "gres",
	.has_arg = required_argument,
	.val = LONG_OPT_GRES,
	.set_func = arg_set_gres,
	.get_func = arg_get_gres,
	.reset_func = arg_reset_gres,
	.reset_each_pass = true,
};

static int arg_set_gres_flags(slurm_opt_t *opt, const char *arg)
{
	/* clear both flag options first */
	opt->job_flags &= ~(GRES_DISABLE_BIND|GRES_ENFORCE_BIND);
	if (!xstrcasecmp(arg, "disable-binding")) {
		opt->job_flags |= GRES_DISABLE_BIND;
	} else if (!xstrcasecmp(arg, "enforce-binding")) {
		opt->job_flags |= GRES_ENFORCE_BIND;
	} else {
		error("Invalid --gres-flags specification");
		exit(-1);
	}

	return SLURM_SUCCESS;
}
static char *arg_get_gres_flags(slurm_opt_t *opt)
{
	if (opt->job_flags | GRES_DISABLE_BIND)
		return xstrdup("disable-binding");
	else if (opt->job_flags | GRES_ENFORCE_BIND)
		return xstrdup("enforce-binding");
	return xstrdup("unset");
}
static void arg_reset_gres_flags(slurm_opt_t *opt)
{
	opt->job_flags &= ~(GRES_DISABLE_BIND);
	opt->job_flags &= ~(GRES_ENFORCE_BIND);
}
static slurm_cli_opt_t slurm_opt_gres_flags = {
	.name = "gres-flags",
	.has_arg = required_argument,
	.val = LONG_OPT_GRES_FLAGS,
	.set_func = arg_set_gres_flags,
	.get_func = arg_get_gres_flags,
	.reset_func = arg_reset_gres_flags,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(hint);
static slurm_cli_opt_t slurm_opt_hint = {
	.name = "hint",
	.has_arg = required_argument,
	.val = LONG_OPT_HINT,
	.set_func = arg_set_hint,
	.get_func = arg_get_hint,
	.reset_func = arg_reset_hint,
	.reset_each_pass = true,
};

COMMON_BOOL_OPTION(hold, "hold");
static slurm_cli_opt_t slurm_opt_hold = {
	.name = "hold",
	.has_arg = no_argument,
	.val = 'H',
	.set_func = arg_set_hold,
	.get_func = arg_get_hold,
	.reset_func = arg_reset_hold,
};

static int arg_set_immediate(slurm_opt_t *opt, const char *arg)
{
	if (opt->sbatch_opt)
		return SLURM_ERROR;

	if (arg)
		opt->immediate = parse_int("immediate", arg, false);
	else
		opt->immediate = DEFAULT_IMMEDIATE;

	return SLURM_SUCCESS;
}
COMMON_INT_OPTION_GET_AND_RESET(immediate);
static slurm_cli_opt_t slurm_opt_immediate = {
	.name = "immediate",
	.has_arg = optional_argument,
	.val = 'I',
	.set_func_salloc = arg_set_immediate,
	.set_func_srun = arg_set_immediate,
	.get_func = arg_get_immediate,
	.reset_func = arg_reset_immediate,
};

static int arg_set_job_name(slurm_opt_t *opt, const char *arg)
{
	if (opt->srun_opt)
		opt->srun_opt->job_name_set_cmd = true;

	xfree(opt->job_name);
	opt->job_name = xstrdup(arg);

	return SLURM_SUCCESS;
}
COMMON_STRING_OPTION_GET(job_name);
static void arg_reset_job_name(slurm_opt_t *opt)
{
	if (opt->srun_opt)
		opt->srun_opt->job_name_set_cmd = false;
	xfree(opt->job_name);
}
static slurm_cli_opt_t slurm_opt_job_name = {
	.name = "job-name",
	.has_arg = required_argument,
	.val = 'J',
	.set_func = arg_set_job_name,
	.get_func = arg_get_job_name,
	.reset_func = arg_reset_job_name,
};

static int arg_set_kill_command(slurm_opt_t *opt, const char *arg)
{
	if (!opt->salloc_opt)
		return SLURM_ERROR;

	/* Optional argument, enables default of SIGTERM if not given. */
	if (!arg) {
		opt->salloc_opt->kill_command_signal = SIGTERM;
		return SLURM_SUCCESS;
	}

	if (!(opt->salloc_opt->kill_command_signal = sig_name2num(arg))) {
		error("Invalid --kill-command specification");
		exit(-1);
	}

	return SLURM_SUCCESS;
}
static char *arg_get_kill_command(slurm_opt_t *opt)
{
	if (!opt->salloc_opt)
		return NULL;

	return sig_num2name(opt->salloc_opt->kill_command_signal);
}
static void arg_reset_kill_command(slurm_opt_t *opt)
{
	if (opt->salloc_opt)
		opt->salloc_opt->kill_command_signal = 0;
}
static slurm_cli_opt_t slurm_opt_kill_command = {
	.name = "kill-command",
	.has_arg = optional_argument,
	.val = 'K',
	.set_func_salloc = arg_set_kill_command,
	.get_func = arg_get_kill_command,
	.reset_func = arg_reset_kill_command,
};

COMMON_STRING_OPTION(licenses);
static slurm_cli_opt_t slurm_opt_licenses = {
	.name = "licenses",
	.has_arg = required_argument,
	.val = 'L',
	.set_func = arg_set_licenses,
	.get_func = arg_get_licenses,
	.reset_func = arg_reset_licenses,
	.reset_each_pass = true,
};

static int arg_set_mail_type(slurm_opt_t *opt, const char *arg)
{
	opt->mail_type |= parse_mail_type(arg);
	if (opt->mail_type == INFINITE16) {
		error("Invalid --mail-type specification");
		exit(-1);
	}

	return SLURM_SUCCESS;
}
static char *arg_get_mail_type(slurm_opt_t *opt)
{
	return xstrdup(print_mail_type(opt->mail_type));
}
COMMON_OPTION_RESET(mail_type, 0);
static slurm_cli_opt_t slurm_opt_mail_type = {
	.name = "mail-type",
	.has_arg = required_argument,
	.val = LONG_OPT_MAIL_TYPE,
	.set_func = arg_set_mail_type,
	.get_func = arg_get_mail_type,
	.reset_func = arg_reset_mail_type,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(mail_user);
static slurm_cli_opt_t slurm_opt_mail_user = {
	.name = "mail-user",
	.has_arg = required_argument,
	.val = LONG_OPT_MAIL_USER,
	.set_func = arg_set_mail_user,
	.get_func = arg_get_mail_user,
	.reset_func = arg_reset_mail_user,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(mcs_label);
static slurm_cli_opt_t slurm_opt_mcs_label = {
	.name = "mcs-label",
	.has_arg = required_argument,
	.val = LONG_OPT_MCS_LABEL,
	.set_func = arg_set_mcs_label,
	.get_func = arg_get_mcs_label,
	.reset_func = arg_reset_mcs_label,
};

static int arg_set_mem(slurm_opt_t *opt, const char *arg)
{
	if ((opt->pn_min_memory = str_to_mbytes2(arg)) == NO_VAL64) {
		error("Invalid --mem specification");
		exit(-1);
	}

	/*
	 * FIXME: the srun command silently stomps on any --mem-per-cpu
	 * setting, as it was likely inherited from the env var.
	 */
	if (opt->srun_opt)
		opt->mem_per_cpu = NO_VAL64;

	return SLURM_SUCCESS;
}
COMMON_MBYTES_OPTION_GET_AND_RESET(pn_min_memory);
static slurm_cli_opt_t slurm_opt_mem = {
	.name = "mem",
	.has_arg = required_argument,
	.val = LONG_OPT_MEM,
	.set_func = arg_set_mem,
	.get_func = arg_get_pn_min_memory,
	.reset_func = arg_reset_pn_min_memory,
};

static int arg_set_mem_bind(slurm_opt_t *opt, const char *arg)
{
	xfree(opt->mem_bind);
	if (slurm_verify_mem_bind(arg, &opt->mem_bind, &opt->mem_bind_type))
		exit(-1);

	return SLURM_SUCCESS;
}
static char *arg_get_mem_bind(slurm_opt_t *opt)
{
	char *tmp;
	if (!opt->mem_bind_type)
		return xstrdup("unset");
	tmp = slurm_xstr_mem_bind_type(opt->mem_bind_type);
	if (opt->mem_bind)
		xstrfmtcat(tmp, ":%s", opt->mem_bind);
	return tmp;
}
static void arg_reset_mem_bind(slurm_opt_t *opt)
{
	xfree(opt->mem_bind);
	opt->mem_bind_type = 0;

	if (opt->srun_opt) {
		char *launch_params = slurm_get_launch_params();
		if (xstrstr(launch_params, "mem_sort"))
			opt->mem_bind_type |= MEM_BIND_SORT;
		xfree(launch_params);
	}
}
static slurm_cli_opt_t slurm_opt_mem_bind = {
	.name = "mem-bind",
	.has_arg = required_argument,
	.val = LONG_OPT_MEM_BIND,
	.set_func = arg_set_mem_bind,
	.get_func = arg_get_mem_bind,
	.reset_func = arg_reset_mem_bind,
	.reset_each_pass = true,
};

COMMON_MBYTES_OPTION(mem_per_cpu, "--mem-per-cpu");
static slurm_cli_opt_t slurm_opt_mem_per_cpu = {
	.name = "mem-per-cpu",
	.has_arg = required_argument,
	.val = LONG_OPT_MEM_PER_CPU,
	.set_func = arg_set_mem_per_cpu,
	.get_func = arg_get_mem_per_cpu,
	.reset_func = arg_reset_mem_per_cpu,
	.reset_each_pass = true,
};

COMMON_MBYTES_OPTION(mem_per_gpu, "--mem-per-gpu");
static slurm_cli_opt_t slurm_opt_mem_per_gpu = {
	.name = "mem-per-gpu",
	.has_arg = required_argument,
	.val = LONG_OPT_MEM_PER_GPU,
	.set_func = arg_set_mem_per_gpu,
	.get_func = arg_get_mem_per_gpu,
	.reset_func = arg_reset_mem_per_gpu,
};

COMMON_INT_OPTION_SET(pn_min_cpus, "--mincpus");
COMMON_INT_OPTION_GET(pn_min_cpus);
COMMON_OPTION_RESET(pn_min_cpus, -1);
static slurm_cli_opt_t slurm_opt_mincpus = {
	.name = "mincpus",
	.has_arg = required_argument,
	.val = LONG_OPT_MINCPUS,
	.set_func = arg_set_pn_min_cpus,
	.get_func = arg_get_pn_min_cpus,
	.reset_func = arg_reset_pn_min_cpus,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(network);
static slurm_cli_opt_t slurm_opt_network = {
	.name = "network",
	.has_arg = required_argument,
	.val = LONG_OPT_NETWORK,
	.set_func = arg_set_network,
	.get_func = arg_get_network,
	.reset_func = arg_reset_network,
	.reset_each_pass = true,
};

static int arg_set_nice(slurm_opt_t *opt, const char *arg)
{
	long long tmp_nice;

	if (arg)
		tmp_nice = strtoll(arg, NULL, 10);
	else
		tmp_nice = 100;

	if (llabs(tmp_nice) > (NICE_OFFSET - 3)) {
		error("Invalid --nice value, out of range (+/- %u)",
		      NICE_OFFSET - 3);
		exit(-1);
	}

	opt->nice = (int) tmp_nice;

	return SLURM_SUCCESS;
}
static char *arg_get_nice(slurm_opt_t *opt)
{
	return xstrdup_printf("%d", opt->nice);
}
COMMON_OPTION_RESET(nice, NO_VAL);
static slurm_cli_opt_t slurm_opt_nice = {
	.name = "nice",
	.has_arg = optional_argument,
	.val = LONG_OPT_NICE,
	.set_func = arg_set_nice,
	.get_func = arg_get_nice,
	.reset_func = arg_reset_nice,
};

/* See --bell above as well */
static int arg_set_no_bell(slurm_opt_t *opt, const char *arg)
{
	if (opt->salloc_opt)
		opt->salloc_opt->bell = BELL_NEVER;

	return SLURM_SUCCESS;
}
static slurm_cli_opt_t slurm_opt_no_bell = {
	.name = "no-bell",
	.has_arg = no_argument,
	.val = LONG_OPT_NO_BELL,
	.set_func_salloc = arg_set_no_bell,
	.get_func = arg_get_bell,
	.reset_func = arg_reset_bell,
};

static int arg_set_no_kill(slurm_opt_t *opt, const char *arg)
{
	if (!arg || !xstrcasecmp(arg, "set"))
		opt->no_kill = true;
	else if (!xstrcasecmp(arg, "off") || !xstrcasecmp(arg, "no"))
		opt->no_kill = false;
	else {
		error("Invalid --no-kill specification");
		exit(-1);
	}

	return SLURM_SUCCESS;
}
static char *arg_get_no_kill(slurm_opt_t *opt)
{
	return xstrdup(opt->no_kill ? "set" : "unset");
}
COMMON_OPTION_RESET(no_kill, false);
static slurm_cli_opt_t slurm_opt_no_kill = {
	.name = "no-kill",
	.has_arg = optional_argument,
	.val = 'k',
	.set_func = arg_set_no_kill,
	.get_func = arg_get_no_kill,
	.reset_func = arg_reset_no_kill,
};

/*
 * FIXME: --nodefile and --nodelist options should be mutually exclusive.
 * Right now they'll overwrite one another; the last to run wins.
 */
static int arg_set_nodefile(slurm_opt_t *opt, const char *arg)
{
	xfree(opt->nodefile);
	xfree(opt->nodelist);
	opt->nodefile = xstrdup(arg);

	return SLURM_SUCCESS;
}
COMMON_STRING_OPTION_GET_AND_RESET(nodefile);
static slurm_cli_opt_t slurm_opt_nodefile = {
	.name = "nodefile",
	.has_arg = required_argument,
	.val = 'F',
	.set_func = arg_set_nodefile,
	.get_func = arg_get_nodefile,
	.reset_func = arg_reset_nodefile,
	.reset_each_pass = true,
};

static int arg_set_nodelist(slurm_opt_t *opt, const char *arg)
{
	xfree(opt->nodefile);
	xfree(opt->nodelist);
	opt->nodelist = xstrdup(arg);

	return SLURM_SUCCESS;
}
COMMON_STRING_OPTION_GET_AND_RESET(nodelist);
static slurm_cli_opt_t slurm_opt_nodelist = {
	.name = "nodelist",
	.has_arg = required_argument,
	.val = 'w',
	.set_func = arg_set_nodelist,
	.get_func = arg_get_nodelist,
	.reset_func = arg_reset_nodelist,
	.reset_each_pass = true,
};

COMMON_BOOL_OPTION(overcommit, "overcommit");
static slurm_cli_opt_t slurm_opt_overcommit = {
	.name = "overcommit",
	.has_arg = no_argument,
	.val = 'O',
	.set_func = arg_set_overcommit,
	.get_func = arg_get_overcommit,
	.reset_func = arg_reset_overcommit,
	.reset_each_pass = true,
};

/*
 * This option is directly tied to --exclusive. Both use the same output
 * function, and the string arguments are designed to mirror one another.
 */
static int arg_set_oversubscribe(slurm_opt_t *opt, const char *arg)
{
	if (opt->srun_opt)
		opt->srun_opt->exclusive = false;

	opt->shared = JOB_SHARED_OK;

	return SLURM_SUCCESS;
}
static slurm_cli_opt_t slurm_opt_oversubscribe = {
	.name = "oversubscribe",
	.has_arg = no_argument,
	.val = 's',
	.set_func = arg_set_oversubscribe,
	.get_func = arg_get_exclusive,
	.reset_func = arg_reset_shared,
	.reset_each_pass = true,
};

COMMON_STRING_OPTION(partition);
static slurm_cli_opt_t slurm_opt_partition = {
	.name = "partition",
	.has_arg = required_argument,
	.val = 'p',
	.set_func = arg_set_partition,
	.get_func = arg_get_partition,
	.reset_func = arg_reset_partition,
	.reset_each_pass = true,
};

static int arg_set_power(slurm_opt_t *opt, const char *arg)
{
	opt->power = power_flags_id(arg);

	return SLURM_SUCCESS;
}
static char *arg_get_power(slurm_opt_t *opt)
{
	if (opt->power)
		return xstrdup(power_flags_str(opt->power));
	return xstrdup("unset");
}
COMMON_OPTION_RESET(power, 0);
static slurm_cli_opt_t slurm_opt_power = {
	.name = "power",
	.has_arg = required_argument,
	.val = LONG_OPT_POWER,
	.set_func = arg_set_power,
	.get_func = arg_get_power,
	.reset_func = arg_reset_power,
	.reset_each_pass = true,
};

static int arg_set_priority(slurm_opt_t *opt, const char *arg)
{
	if (!xstrcasecmp(arg, "TOP")) {
		opt->priority = NO_VAL - 1;
	} else {
		long long priority = strtoll(arg, NULL, 10);
		if (priority < 0) {
			error("Priority must be >= 0");
			exit(-1);
		}
		if (priority >= NO_VAL) {
			error("Priority must be < %u", NO_VAL);
			exit(-1);
		}
		opt->priority = priority;
	}

	return SLURM_SUCCESS;
}
COMMON_INT_OPTION_GET_AND_RESET(priority);
static slurm_cli_opt_t slurm_opt_priority = {
	.name = "priority",
	.has_arg = required_argument,
	.val = LONG_OPT_PRIORITY,
	.set_func = arg_set_priority,
	.get_func = arg_get_priority,
	.reset_func = arg_reset_priority,
};

static int arg_set_profile(slurm_opt_t *opt, const char *arg)
{
	opt->profile = acct_gather_profile_from_string(arg);

	return SLURM_SUCCESS;
}
static char *arg_get_profile(slurm_opt_t *opt)
{
	return xstrdup(acct_gather_profile_to_string(opt->profile));
}
COMMON_OPTION_RESET(profile, ACCT_GATHER_PROFILE_NOT_SET);
static slurm_cli_opt_t slurm_opt_profile = {
	.name = "profile",
	.has_arg = required_argument,
	.val = LONG_OPT_PROFILE,
	.set_func = arg_set_profile,
	.get_func = arg_get_profile,
	.reset_func = arg_reset_profile,
};

COMMON_STRING_OPTION(qos);
static slurm_cli_opt_t slurm_opt_qos = {
	.name = "qos",
	.has_arg = required_argument,
	.val = 'q',
	.set_func = arg_set_qos,
	.get_func = arg_get_qos,
	.reset_func = arg_reset_qos,
};

COMMON_BOOL_OPTION(reboot, "reboot");
static slurm_cli_opt_t slurm_opt_reboot = {
	.name = "reboot",
	.has_arg = no_argument,
	.val = LONG_OPT_REBOOT,
	.set_func = arg_set_reboot,
	.get_func = arg_get_reboot,
	.reset_func = arg_reset_reboot,
};

COMMON_STRING_OPTION(reservation);
static slurm_cli_opt_t slurm_opt_reservation = {
	.name = "reservation",
	.has_arg = required_argument,
	.val = LONG_OPT_RESERVATION,
	.set_func = arg_set_reservation,
	.get_func = arg_get_reservation,
	.reset_func = arg_reset_reservation,
};

static int arg_set_signal(slurm_opt_t *opt, const char *arg)
{
	if (get_signal_opts((char *) arg, &opt->warn_signal,
			    &opt->warn_time, &opt->warn_flags)) {
		error("Invalid --signal specification");
		exit(-1);
	}

	return SLURM_SUCCESS;
}
static char *arg_get_signal(slurm_opt_t *opt)
{
	return signal_opts_to_cmdline(opt->warn_signal, opt->warn_time,
				      opt->warn_flags);
}
static void arg_reset_signal(slurm_opt_t *opt)
{
	opt->warn_flags = 0;
	opt->warn_signal = 0;
	opt->warn_time = 0;
}
static slurm_cli_opt_t slurm_opt_signal = {
	.name = "signal",
	.has_arg = required_argument,
	.val = LONG_OPT_SIGNAL,
	.set_func = arg_set_signal,
	.get_func = arg_get_signal,
	.reset_func = arg_reset_signal,
};

static int arg_set_spread_job(slurm_opt_t *opt, const char *arg)
{
	opt->job_flags |= SPREAD_JOB;

	return SLURM_SUCCESS;
}
static char *arg_get_spread_job(slurm_opt_t *opt)
{
	if (opt->job_flags | SPREAD_JOB)
		return xstrdup("set");
	return xstrdup("unset");
}
static void arg_reset_spread_job(slurm_opt_t *opt)
{
	opt->job_flags &= ~SPREAD_JOB;
}
static slurm_cli_opt_t slurm_opt_spread_job = {
	.name = "spread-job",
	.has_arg = no_argument,
	.val = LONG_OPT_SPREAD_JOB,
	.set_func = arg_set_spread_job,
	.get_func = arg_get_spread_job,
	.reset_func = arg_reset_spread_job,
	.reset_each_pass = true,
};

static int arg_set_switch_req(slurm_opt_t *opt, const char *arg)
{
	opt->req_switch = parse_int("--switches", arg, true);

	return SLURM_SUCCESS;
}
static char *arg_get_switch_req(slurm_opt_t *opt)
{
	if (opt->req_switch != -1)
		return xstrdup_printf("%d", opt->req_switch);
	return xstrdup("unset");
}
static void arg_reset_switch_req(slurm_opt_t *opt)
{
	opt->req_switch = -1;
}
static slurm_cli_opt_t slurm_opt_switch_req = {
	.name = NULL, /* envvar only */
	.has_arg = required_argument,
	.val = LONG_OPT_SWITCH_REQ,
	.set_func = arg_set_switch_req,
	.get_func = arg_get_switch_req,
	.reset_func = arg_reset_switch_req,
	.reset_each_pass = true,
};

static int arg_set_switch_wait(slurm_opt_t *opt, const char *arg)
{
	opt->wait4switch = time_str2secs(arg);

	return SLURM_SUCCESS;
}
static char *arg_get_switch_wait(slurm_opt_t *opt)
{
	char time_str[32];
	secs2time_str(opt->wait4switch, time_str, sizeof(time_str));
	return xstrdup_printf("%s", time_str);
}
static void arg_reset_switch_wait(slurm_opt_t *opt)
{
	opt->req_switch = -1;
	opt->wait4switch = -1;
}
static slurm_cli_opt_t slurm_opt_switch_wait = {
	.name = NULL, /* envvar only */
	.has_arg = required_argument,
	.val = LONG_OPT_SWITCH_WAIT,
	.set_func = arg_set_switch_wait,
	.get_func = arg_get_switch_wait,
	.reset_func = arg_reset_switch_wait,
	.reset_each_pass = true,
};

static int arg_set_switches(slurm_opt_t *opt, const char *arg)
{
	char *tmparg = xstrdup(arg);
	char *split = xstrchr(tmparg, '@');

	if (split) {
		split[0] = '\0';
		split++;
		opt->wait4switch = time_str2secs(split);
	}

	opt->req_switch = parse_int("--switches", tmparg, true);

	xfree(tmparg);

	return SLURM_SUCCESS;
}
static char *arg_get_switches(slurm_opt_t *opt)
{
	if (opt->wait4switch != -1) {
		char time_str[32];
		secs2time_str(opt->wait4switch, time_str, sizeof(time_str));
		return xstrdup_printf("%d@%s", opt->req_switch, time_str);
	}
	if (opt->req_switch != -1)
		return xstrdup_printf("%d", opt->req_switch);
	return xstrdup("unset");
}
static void arg_reset_switches(slurm_opt_t *opt)
{
	opt->req_switch = -1;
	opt->wait4switch = -1;
}
static slurm_cli_opt_t slurm_opt_switches = {
	.name = "switches",
	.has_arg = required_argument,
	.val = LONG_OPT_SWITCHES,
	.set_func = arg_set_switches,
	.get_func = arg_get_switches,
	.reset_func = arg_reset_switches,
	.reset_each_pass = true,
};

/* note this is mutually exclusive with --core-spec above */
static int arg_set_thread_spec(slurm_opt_t *opt, const char *arg)
{
	opt->core_spec = parse_int("--thread-spec", arg, true);
	opt->core_spec |= CORE_SPEC_THREAD;

	return SLURM_SUCCESS;
}
static char *arg_get_thread_spec(slurm_opt_t *opt)
{
	if ((opt->core_spec == NO_VAL16) ||
	    !(opt->core_spec & CORE_SPEC_THREAD))
		return xstrdup("unset");
	return xstrdup_printf("%d", (opt->core_spec & ~CORE_SPEC_THREAD));
}
static slurm_cli_opt_t slurm_opt_thread_spec = {
	.name = "thread-spec",
	.has_arg = required_argument,
	.val = LONG_OPT_THREAD_SPEC,
	.set_func = arg_set_thread_spec,
	.get_func = arg_get_thread_spec,
	.reset_func = arg_reset_core_spec,
	.reset_each_pass = true,
};

static int arg_set_time_limit(slurm_opt_t *opt, const char *arg)
{
	int time_limit;

	time_limit = time_str2mins(arg);
	if (time_limit == NO_VAL) {
		error("Invalid --time specification");
		exit(-1);
	} else if (time_limit == 0) {
		time_limit = INFINITE;
	}

	opt->time_limit = time_limit;
	return SLURM_SUCCESS;
}
COMMON_TIME_DURATION_OPTION_GET_AND_RESET(time_limit);
static slurm_cli_opt_t slurm_opt_time_limit = {
	.name = "time",
	.has_arg = required_argument,
	.val = 't',
	.set_func = arg_set_time_limit,
	.get_func = arg_get_time_limit,
	.reset_func = arg_reset_time_limit,
};

static int arg_set_time_min(slurm_opt_t *opt, const char *arg)
{
	int time_min;

	time_min = time_str2mins(arg);
	if (time_min == NO_VAL) {
		error("Invalid --time-min specification");
		exit(-1);
	} else if (time_min == 0) {
		time_min = INFINITE;
	}

	opt->time_min = time_min;
	return SLURM_SUCCESS;
}
COMMON_TIME_DURATION_OPTION_GET_AND_RESET(time_min);
static slurm_cli_opt_t slurm_opt_time_min = {
	.name = "time-min",
	.has_arg = required_argument,
	.val = LONG_OPT_TIME_MIN,
	.set_func = arg_set_time_min,
	.get_func = arg_get_time_min,
	.reset_func = arg_reset_time_min,
};

COMMON_MBYTES_OPTION(pn_min_tmp_disk, "--tmp");
static slurm_cli_opt_t slurm_opt_tmp = {
	.name = "tmp",
	.has_arg = required_argument,
	.val = LONG_OPT_TMP,
	.set_func = arg_set_pn_min_tmp_disk,
	.get_func = arg_get_pn_min_tmp_disk,
	.reset_func = arg_reset_pn_min_tmp_disk,
	.reset_each_pass = true,
};

static int arg_set_use_min_nodes(slurm_opt_t *opt, const char *arg)
{
	opt->job_flags |= USE_MIN_NODES;

	return SLURM_SUCCESS;
}
static char *arg_get_use_min_nodes(slurm_opt_t *opt)
{
	if (opt->job_flags | USE_MIN_NODES)
		return xstrdup("set");
	return xstrdup("unset");
}
static void arg_reset_use_min_nodes(slurm_opt_t *opt)
{
	opt->job_flags &= ~(USE_MIN_NODES);
}
static slurm_cli_opt_t slurm_opt_use_min_nodes = {
	.name = "use-min-nodes",
	.has_arg = no_argument,
	.val = LONG_OPT_USE_MIN_NODES,
	.set_func = arg_set_use_min_nodes,
	.get_func = arg_get_use_min_nodes,
	.reset_func = arg_reset_use_min_nodes,
	.reset_each_pass = true,
};

static int arg_set_verbose(slurm_opt_t *opt, const char *arg)
{
	/*
	 * Note that verbose is handled a bit differently. As a cli argument,
	 * it has no_argument set so repeated 'v' characters can be used.
	 * As an environment variable though, it will have a numeric value.
	 * The boolean treatment from slurm_process_option() will still pass
	 * the string form along to us, which we can parse here into the
	 * correct value.
	 */
	if (!arg)
		opt->verbose++;
	else
		opt->verbose = parse_int("--verbose", arg, false);

	return SLURM_SUCCESS;
}
COMMON_INT_OPTION_GET_AND_RESET(verbose);
static slurm_cli_opt_t slurm_opt_verbose = {
	.name = "verbose",
	.has_arg = no_argument,	/* sort of */
	.val = 'v',
	.sbatch_early_pass = true,
	.set_func = arg_set_verbose,
	.get_func = arg_get_verbose,
	.reset_func = arg_reset_verbose,
};

static int arg_set_wait_all_nodes(slurm_opt_t *opt, const char *arg)
{
	uint16_t tmp;

	if (!opt->salloc_opt && !opt->sbatch_opt)
		return SLURM_ERROR;

	tmp = parse_int("--wait-all-nodes", arg, false);

	if ((tmp < 0) || (tmp > 1)) {
		error("Invalid --wait-all-nodes specification");
		exit(-1);
	}

	if (opt->salloc_opt)
		opt->salloc_opt->wait_all_nodes = tmp;
	if (opt->sbatch_opt)
		opt->sbatch_opt->wait_all_nodes = tmp;

	return SLURM_SUCCESS;
}
static char *arg_get_wait_all_nodes(slurm_opt_t *opt)
{
	uint16_t tmp;

	if (!opt->salloc_opt && !opt->sbatch_opt)
		return xstrdup("invalid-context");

	if (opt->salloc_opt)
		tmp = opt->salloc_opt->wait_all_nodes;
	if (opt->sbatch_opt)
		tmp = opt->sbatch_opt->wait_all_nodes;

	return xstrdup_printf("%u", tmp);
}
static void arg_reset_wait_all_nodes(slurm_opt_t *opt)
{
	if (opt->salloc_opt)
		opt->salloc_opt->wait_all_nodes = NO_VAL16;
	if (opt->sbatch_opt)
		opt->sbatch_opt->wait_all_nodes = NO_VAL16;
}
static slurm_cli_opt_t slurm_opt_wait_all_nodes = {
	.name = "wait-all-nodes",
	.has_arg = required_argument,
	.val = LONG_OPT_WAIT_ALL_NODES,
	.set_func_salloc = arg_set_wait_all_nodes,
	.set_func_sbatch = arg_set_wait_all_nodes,
	.get_func = arg_get_wait_all_nodes,
	.reset_func = arg_reset_wait_all_nodes,
};

COMMON_STRING_OPTION(wckey);
static slurm_cli_opt_t slurm_opt_wckey = {
	.name = "wckey",
	.has_arg = required_argument,
	.val = LONG_OPT_WCKEY,
	.set_func = arg_set_wckey,
	.get_func = arg_get_wckey,
	.reset_func = arg_reset_wckey,
};

static slurm_cli_opt_t *common_options[] = {
	&slurm_opt_account,
	&slurm_opt_acctg_freq,
	&slurm_opt_begin,
	&slurm_opt_bell,
	&slurm_opt_bb,
	&slurm_opt_c_constraint,
	&slurm_opt_chdir,
	&slurm_opt_cluster,
	&slurm_opt_clusters,
	&slurm_opt_comment,
	&slurm_opt_contiguous,
	&slurm_opt_constraint,
	&slurm_opt_core_spec,
	&slurm_opt_cpu_freq,
	&slurm_opt_cpus_per_gpu,
	&slurm_opt_deadline,
	&slurm_opt_delay_boot,
	&slurm_opt_dependency,
	&slurm_opt_distribution,
	&slurm_opt_exclude,
	&slurm_opt_exclusive,
	&slurm_opt_extra_node_info,
	&slurm_opt_get_user_env,
	&slurm_opt_gpu_bind,
	&slurm_opt_gpu_freq,
	&slurm_opt_gpus,
	&slurm_opt_gpus_per_node,
	&slurm_opt_gpus_per_socket,
	&slurm_opt_gpus_per_task,
	&slurm_opt_gres,
	&slurm_opt_gres_flags,
	&slurm_opt_hint,
	&slurm_opt_hold,
	&slurm_opt_immediate,
	&slurm_opt_job_name,
	&slurm_opt_kill_command,
	&slurm_opt_licenses,
	&slurm_opt_mail_type,
	&slurm_opt_mail_user,
	&slurm_opt_mcs_label,
	&slurm_opt_mem,
	&slurm_opt_mem_bind,
	&slurm_opt_mem_per_cpu,
	&slurm_opt_mem_per_gpu,
	&slurm_opt_mincpus,
	&slurm_opt_network,
	&slurm_opt_nice,
	&slurm_opt_no_bell,
	&slurm_opt_no_kill,
	&slurm_opt_nodefile,
	&slurm_opt_nodelist,
	&slurm_opt_overcommit,
	&slurm_opt_oversubscribe,
	&slurm_opt_partition,
	&slurm_opt_power,
	&slurm_opt_priority,
	&slurm_opt_profile,
	&slurm_opt_qos,
	&slurm_opt_reboot,
	&slurm_opt_reservation,
	&slurm_opt_signal,
	&slurm_opt_spread_job,
	&slurm_opt_switch_req,
	&slurm_opt_switch_wait,
	&slurm_opt_switches,
	&slurm_opt_thread_spec,
	&slurm_opt_time_limit,
	&slurm_opt_time_min,
	&slurm_opt_tmp,
	&slurm_opt_use_min_nodes,
	&slurm_opt_verbose,
	&slurm_opt_wait_all_nodes,
	&slurm_opt_wckey,
	NULL /* END */
};

struct option *slurm_option_table_create(struct option *options,
					 slurm_opt_t *opt)
{
	struct option *merged = optz_create();

	optz_append(&merged, options);
	/*
	 * Since the initial elements of slurm_cli_opt_t match
	 * the layout of struct option, we can use this cast to
	 * avoid needing to make a temporary structure.
	 */
	for (int i = 0; common_options[i]; i++) {
		bool set = true;
		/*
		 * Runtime sanity checking for development builds,
		 * as I cannot find a convenient way to instruct the
		 * compiler to handle this. So if you make a mistake,
		 * you'll hit an assertion failure in salloc/srun/sbatch.
		 *
		 * If set_func is set, the others must not be:
		 */
		xassert((common_options[i]->set_func
			 && !common_options[i]->set_func_salloc
			 && !common_options[i]->set_func_sbatch
			 && !common_options[i]->set_func_srun) ||
			!common_options[i]->set_func);
		/*
		 * These two must always be set:
		 */
		xassert(common_options[i]->get_func);
		xassert(common_options[i]->reset_func);

		/*
		 * A few options only exist as environment variables, and
		 * should not be added to the table. They should be marked
		 * with a NULL name field.
		 */
		if (!common_options[i]->name)
			continue;

		if (common_options[i]->set_func)
			optz_add(&merged, (struct option *) common_options[i]);
		else if (opt->salloc_opt && common_options[i]->set_func_salloc)
			optz_add(&merged, (struct option *) common_options[i]);
		else if (opt->sbatch_opt && common_options[i]->set_func_sbatch)
			optz_add(&merged, (struct option *) common_options[i]);
		else if (opt->srun_opt && common_options[i]->set_func_srun)
			optz_add(&merged, (struct option *) common_options[i]);
		else
			set = false;

		if (set) {
			/* FIXME: append appropriate characters to optstring */
		}
	}
	return merged;
}

void slurm_option_table_destroy(struct option *optz)
{
	optz_destroy(optz);
}

int slurm_process_option(slurm_opt_t *opt, int optval, const char *arg,
			 bool set_by_env, bool early_pass)
{
	int i;
	const char *setarg = arg;
	bool set = true;

	if (!opt)
		fatal("%s: missing slurm_opt_t struct", __func__);

	for (i = 0; common_options[i]; i++) {
		if (common_options[i]->val != optval)
			continue;

		/* Check that this is a valid match. */
		if (!common_options[i]->set_func &&
		    !(opt->salloc_opt && common_options[i]->set_func_salloc) &&
		    !(opt->sbatch_opt && common_options[i]->set_func_sbatch) &&
		    !(opt->srun_opt && common_options[i]->set_func_srun))
			continue;

		/* Match found */
		break;
	}

	if (!common_options[i])
		return SLURM_ERROR;

	/*
	 * Special handling for the early pass in sbatch.
	 *
	 * Some options are handled in the early pass, but most are deferred
	 * to a later pass, in which case those options are not re-evaluated.
	 * Environment variables are always evaluated by this though - there
	 * is no distinction for them of early vs normal passes.
	 */
	if (!set_by_env && opt->sbatch_opt) {
		if (!early_pass && common_options[i]->sbatch_early_pass)
			return SLURM_SUCCESS;
		if (early_pass && !common_options[i]->sbatch_early_pass)
			return SLURM_SUCCESS;
	}

	if (arg) {
		if (common_options[i]->has_arg == no_argument) {
			char *end;
			/*
			 * Treat these "flag" arguments specially.
			 * For normal getopt_long() handling, arg is null.
			 * But for envvars, arg may be set, and will be
			 * processed by these rules:
			 * arg == '\0', flag is set
			 * arg == "yes", flag is set
			 * arg is a non-zero number, flag is set
			 * arg is anything else, call reset instead
			 */
			if (arg[0] == '\0') {
				set = true;
			} else if (!xstrcasecmp(arg, "yes")) {
				set = true;
			} else if (strtol(arg, &end, 10) && (*end == '\0')) {
				set = true;
			} else {
				set = false;
			}
		} else if (common_options[i]->has_arg == required_argument) {
			/* no special processing required */
		} else if (common_options[i]->has_arg == optional_argument) {
			/*
			 * If an empty string, convert to null,
			 * as this will let the envvar processing
			 * match the normal getopt_long() behavior.
			 */
			if (arg[0] == '\0')
				setarg = NULL;
		}
	}

	if (!set) {
		(common_options[i]->reset_func)(opt);
		common_options[i]->set = false;
		common_options[i]->set_by_env = false;
		return SLURM_SUCCESS;
	}

	if (common_options[i]->set_func) {
		if (!(common_options[i]->set_func)(opt, setarg)) {
			common_options[i]->set = true;
			common_options[i]->set_by_env = set_by_env;
			return SLURM_SUCCESS;
		}
	} else if (opt->salloc_opt && common_options[i]->set_func_salloc) {
		if (!(common_options[i]->set_func_salloc)(opt, setarg)) {
			common_options[i]->set = true;
			common_options[i]->set_by_env = set_by_env;
			return SLURM_SUCCESS;
		}
	} else if (opt->sbatch_opt && common_options[i]->set_func_sbatch) {
		if (!(common_options[i]->set_func_sbatch)(opt, setarg)) {
			common_options[i]->set = true;
			common_options[i]->set_by_env = set_by_env;
			return SLURM_SUCCESS;
		}
	} else if (opt->srun_opt && common_options[i]->set_func_srun) {
		if (!(common_options[i]->set_func_srun)(opt, setarg)) {
			common_options[i]->set = true;
			common_options[i]->set_by_env = set_by_env;
			return SLURM_SUCCESS;
		}
	}
	return SLURM_ERROR;
}

void slurm_print_set_options(slurm_opt_t *opt)
{
	if (!opt)
		fatal("%s: missing slurm_opt_t struct", __func__);

	info("defined options");
	info("-------------------- --------------------");

	for (int i = 0; common_options[i]; i++) {
		char *val = NULL;

		if (!common_options[i]->set)
			continue;

		if (common_options[i]->get_func)
			val = (common_options[i]->get_func)(opt);
		info("%-20s: %s", common_options[i]->name, val);
		xfree(val);
	}
	info("-------------------- --------------------");
	info("end of defined options");
}

extern void slurm_reset_all_options(slurm_opt_t *opt, bool first_pass)
{
	for (int i = 0; common_options[i]; i++) {
		if (!first_pass && !common_options[i]->reset_each_pass)
			continue;
		if (common_options[i]->reset_func) {
			(common_options[i]->reset_func)(opt);
			common_options[i]->set = false;
		}
	}
}

/*
 * Was the option set by an env var?
 */
extern bool slurm_option_set_by_env(int optval)
{
	int i;

	for (i = 0; common_options[i]; i++) {
		if (common_options[i]->val == optval)
			break;
	}

	/* This should not happen... */
	if (!common_options[i])
		return false;

	return common_options[i]->set_by_env;
}
