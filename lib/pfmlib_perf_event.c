/*
 * pfmlib_perf_events.c: encode events for perf_event API
 *
 * Copyright (c) 2009 Google, Inc
 * Contributed by Stephane Eranian <eranian@google.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <perfmon/pfmlib_perf_event.h>
#include "pfmlib_priv.h"

static int
get_perf_event_encoding(const char *str, int dfl_plm, struct perf_event_attr *hw, char **fstr, int *idx)
{
	pfmlib_pmu_t *pmu;
	pfmlib_event_desc_t e;
	pfmlib_perf_attr_t perf_attrs;
	int ret;

	memset(&e, 0, sizeof(e));

	ret = pfmlib_parse_event(str, &e);
	if (ret != PFM_SUCCESS)
		return ret;

	pmu = e.pmu;

	/*
	 * initialize default priv level mask
	 * is used if no plm modifier is passed
	 */
	e.dfl_plm = dfl_plm;

	memset(&perf_attrs, 0, sizeof(perf_attrs));

	/*
	 * values[] dynamically allocated by call because we
	 * pass NULL
	 */
	ret = pmu->get_event_encoding(pmu, &e, &perf_attrs);
	if (ret != PFM_SUCCESS)
		return ret;


	hw->type = pmu->get_event_perf_type(pmu, e.event);
	if (hw->type == -1)
		return PFM_ERR_NOTSUPP;

	hw->config = e.codes[0];

	/*
	 * propagate to event attributes to perf_event
	 * use dfl_plm if no modifier specified
	 */
	if (perf_attrs.plm) {
		hw->exclude_user = !(perf_attrs.plm & PFM_PLM3);
		hw->exclude_kernel = !(perf_attrs.plm & PFM_PLM0);
		hw->exclude_hv = !(perf_attrs.plm & PFM_PLMH);
	} else {
		hw->exclude_user = !(dfl_plm & PFM_PLM3);
		hw->exclude_kernel = !(dfl_plm & PFM_PLM0);
		hw->exclude_hv = !(dfl_plm & PFM_PLMH);
	}

	/*
	 * encoding of Intel Nehalem/Westmere OFFCORE_RESPONSE events
	 * they use an extra MSR, which is encoded in the upper 32 bits
	 * of hw->config
	 */
	if (perf_attrs.offcore) {
		if (e.count != 2) {
			DPRINT("perf_encoding: offcore=1 count=%d\n", e.count);
			return PFM_ERR_INVAL;
		}
		hw->config |= e.codes[1] << 32;
	} else {
		uint64_t escr;
		int is_p4;

		is_p4 = pmu->pmu == PFM_PMU_INTEL_NETBURST
			|| pmu->pmu == PFM_PMU_INTEL_NETBURST_P;


		if (!is_p4 && e.count > 2) {
			DPRINT("cannot handle e.count > 2\n");
			return PFM_ERR_NOTSUPP;
		}
		/*
		 * codes[0] = ESCR
		 * codes[1] = CCCR
		 * codes[2] = P4_EVENTS (perf code)
		 */
		if (is_p4) {
			/* cleanup event_select, and install perf specific code */
			escr  = e.codes[0] & ~(0x3full << 25);
			escr |= e.codes[2] << 25;
			hw->config = (escr << 32) | e.codes[1];
		}
	}

	/*
	 * perf_event precise_ip must be in [0-3]
	 * see perf_event.h
	 */
	if (perf_attrs.precise_ip < 0 || perf_attrs.precise_ip > 3)
		return PFM_ERR_ATTR_SET;

	hw->precise_ip = perf_attrs.precise_ip;

	__pfm_vbprintf("PERF[type=%x val=0x%"PRIx64" e_u=%d e_k=%d e_hv=%d precise=%d] %s\n",
			hw->type,
			hw->config,
			hw->exclude_user,
			hw->exclude_kernel,
			hw->exclude_hv,
			hw->precise_ip,
			e.fstr);

	/*
	 * propagate event index if necessary
	 */
	if (idx)
		*idx = pfmlib_pidx2idx(e.pmu, e.event);

	/*
	 * propagate fully qualified event string if necessary
	 */
	return pfmlib_build_fstr(&e, fstr);
}

int
pfm_get_perf_event_encoding(const char *str, int dfl_plm, struct perf_event_attr *hw, char **fstr, int *idx)
{
	if (PFMLIB_INITIALIZED() == 0)
		return PFM_ERR_NOINIT;

	if (!(hw && str))
		return PFM_ERR_INVAL;

	/* must provide default priv level */
	if (dfl_plm < 1)
		return PFM_ERR_INVAL;

	memset(hw, 0, sizeof(*hw));

	return get_perf_event_encoding(str, dfl_plm, hw, fstr, idx);
}