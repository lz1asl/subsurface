/* calculate deco values
 * based on Bühlmann ZHL-16b
 * based on an implemention by heinrichs weikamp for the DR5
 * the original file doesn't carry a license and is used here with
 * the permission of Matthias Heinrichs
 *
 * The implementation below is (C) Dirk Hohndel 2012 and released under the GPLv2
 *
 * clear_deco()  - call to initialize for a new deco calculation
 * add_segment(pressure, gasmix) - add 1 second at the given pressure, breathing gasmix
 * deco_allowed_depth(tissues_tolerance, surface_pressure, dive, smooth)
 *				- ceiling based on lead tissue, surface pressure, 3m increments or smooth
 */
#include "dive.h"

//! Option structure for Buehlmann decompression.
struct buehlmann_config {
  double  satmult;		//! safety at inert gas accumulation as percentage of effect (more than 100).
  double  desatmult;		//! safety at inert gas depletion as percentage of effect (less than 100).
  double  safety_dist_deco_stop;//! assumed distance to official decompression where decompression takes places.
  int  last_deco_stop_in_mtr;	//! depth of last_deco_stop.
  double  gf_high;		//! gradient factor high (at surface).
  double  gf_low;		//! gradient factor low (at bottom/start of deco calculation).
  double  gf_low_position_min;	//! gf_low_position below surface_min_shallow.
  double  gf_low_position_max;	//! gf_low_position below surface_max_depth.
  double  gf_high_emergency;	//! emergency gf factors
  double  gf_low_emergency;	//! gradient factor low (at bottom/start of deco calculation).
};
struct buehlmann_config buehlmann_config = { 1.0, 1.01, 0.5, 3, 75.0, 35.0, 10.0, 30.0, 95.0, 95.0 };
struct dive_data {
        double pressure;	//! pesent ambient pressure
        double surface;		//! pressure at water surface
        struct gasmix *gasmix;	//! current selected gas
};

const double buehlmann_N2_a[] = {1.1696, 1.0, 0.8618, 0.7562,
				 0.62, 0.5043, 0.441, 0.4,
				 0.375, 0.35, 0.3295, 0.3065,
				 0.2835, 0.261, 0.248, 0.2327};

const double buehlmann_N2_b[] = {0.5578, 0.6514, 0.7222, 0.7825,
				 0.8126, 0.8434, 0.8693, 0.8910,
				 0.9092, 0.9222, 0.9319, 0.9403,
				 0.9477, 0.9544, 0.9602, 0.9653};

const double buehlmann_N2_t_halflife[] = {5.0, 8.0, 12.5, 18.5,
					 27.0, 38.3, 54.3, 77.0,
					 109.0, 146.0, 187.0, 239.0,
					 305.0, 390.0, 498.0, 635.0};

const double buehlmann_N2_factor_expositon_one_second[] = {
	2.30782347297664E-003, 1.44301447809736E-003, 9.23769302935806E-004, 6.24261986779007E-004,
	4.27777107246730E-004, 3.01585140931371E-004, 2.12729727268379E-004, 1.50020603047807E-004,
	1.05980191127841E-004, 7.91232600646508E-005, 6.17759153688224E-005, 4.83354552742732E-005,
	3.78761777920511E-005, 2.96212356654113E-005, 2.31974277413727E-005, 1.81926738960225E-005};

const double buehlmann_He_a[] = { 1.6189, 1.383 , 1.1919, 1.0458,
				  0.922 , 0.8205, 0.7305, 0.6502,
				  0.595 , 0.5545, 0.5333, 0.5189,
				  0.5181, 0.5176, 0.5172, 0.5119};

const double buehlmann_He_b[] = {0.4770, 0.5747, 0.6527, 0.7223,
				 0.7582, 0.7957, 0.8279, 0.8553,
				 0.8757, 0.8903, 0.8997, 0.9073,
				 0.9122, 0.9171, 0.9217, 0.9267};

const double buehlmann_He_t_halflife[] = {1.88, 3.02, 4.72, 6.99,
					  10.21, 14.48, 20.53, 29.11,
					  41.20, 55.19, 70.69, 90.34,
					  115.29, 147.42, 188.24, 240.03};

const double buehlmann_He_factor_expositon_one_second[] = {
	6.12608039419837E-003, 3.81800836683133E-003, 2.44456078654209E-003, 1.65134647076792E-003,
	1.13084424730725E-003, 7.97503165599123E-004, 5.62552521860549E-004, 3.96776399429366E-004,
	2.80360036664540E-004, 2.09299583354805E-004, 1.63410794820518E-004, 1.27869320250551E-004,
	1.00198406028040E-004, 7.83611475491108E-005, 6.13689891868496E-005, 4.81280465299827E-005};

#define WV_PRESSURE 0.0627 /* water vapor pressure */
#define DIST_FROM_3_MTR 0.28
#define PRESSURE_CHANGE_3M 0.3
#define TOLERANCE 0.02

double tissue_n2_sat[16];
double tissue_he_sat[16];
double tissue_tolerated_ambient_pressure[16];
int ci_pointing_to_guiding_tissue;
double gf_low_position_this_dive;
int divetime;



static double actual_gradient_limit(const struct dive_data *data)
{
	double pressure_diff, limit_at_position;
	double gf_high = buehlmann_config.gf_high;
	double gf_low = buehlmann_config.gf_low;

	pressure_diff = data->pressure - data->surface;

	if (pressure_diff > TOLERANCE) {
		if (pressure_diff < gf_low_position_this_dive)
			limit_at_position = gf_high - ((gf_high - gf_low) * pressure_diff / gf_low_position_this_dive);
		else
			limit_at_position = gf_low;
	} else {
		limit_at_position = gf_high;
	}
	return limit_at_position;
}

static double gradient_factor_calculation(const struct dive_data *data)
{
	double tissue_inertgas_saturation;

	tissue_inertgas_saturation = tissue_n2_sat[ci_pointing_to_guiding_tissue] +
					tissue_he_sat[ci_pointing_to_guiding_tissue];
	if (tissue_inertgas_saturation < data->pressure)
		return 0.0;
	else
		return (tissue_inertgas_saturation - data->pressure) /
			(tissue_inertgas_saturation - tissue_tolerated_ambient_pressure[ci_pointing_to_guiding_tissue]);
}

static double tissue_tolerance_calc(void)
{
	int ci = -1;
	double tissue_inertgas_saturation, buehlmann_inertgas_a, buehlmann_inertgas_b;
	double ret_tolerance_limit_ambient_pressure = -1.0;

	for (ci = 0; ci < 16; ci++)
	{
		tissue_inertgas_saturation = tissue_n2_sat[ci] + tissue_he_sat[ci];
		buehlmann_inertgas_a = ((buehlmann_N2_a[ci] * tissue_n2_sat[ci]) + (buehlmann_He_a[ci] * tissue_he_sat[ci])) / tissue_inertgas_saturation;
		buehlmann_inertgas_b = ((buehlmann_N2_b[ci] * tissue_n2_sat[ci]) + (buehlmann_He_b[ci] * tissue_he_sat[ci])) / tissue_inertgas_saturation;

		tissue_tolerated_ambient_pressure[ci] = (tissue_inertgas_saturation - buehlmann_inertgas_a) * buehlmann_inertgas_b;

		if (tissue_tolerated_ambient_pressure[ci] > ret_tolerance_limit_ambient_pressure)
		{
			ci_pointing_to_guiding_tissue = ci;
			ret_tolerance_limit_ambient_pressure = tissue_tolerated_ambient_pressure[ci];
		}
	}
	return (ret_tolerance_limit_ambient_pressure);
}

/* add a second at the given pressure and gas to the deco calculation */
double add_segment(double pressure, struct gasmix *gasmix)
{
	int ci;
	double ppn2 = (pressure - WV_PRESSURE) * (1000 - gasmix->o2.permille - gasmix->he.permille) / 1000.0;
	double pphe = (pressure - WV_PRESSURE) * gasmix->he.permille / 1000.0;

	divetime++;
	/* right now we just do OC */
	for (ci = 0; ci < 16; ci++) {
		if (ppn2 - tissue_n2_sat[ci] > 0)
			tissue_n2_sat[ci] += buehlmann_config.satmult * (ppn2 - tissue_n2_sat[ci]) * buehlmann_N2_factor_expositon_one_second[ci];
		else
			tissue_n2_sat[ci] += buehlmann_config.desatmult * (ppn2 - tissue_n2_sat[ci]) * buehlmann_N2_factor_expositon_one_second[ci];
		if (pphe - tissue_he_sat[ci] > 0)
			tissue_he_sat[ci] += buehlmann_config.satmult * (pphe - tissue_he_sat[ci]) * buehlmann_He_factor_expositon_one_second[ci];
		else
			tissue_he_sat[ci] += buehlmann_config.desatmult * (pphe - tissue_he_sat[ci]) * buehlmann_He_factor_expositon_one_second[ci];
	}
	return tissue_tolerance_calc();
}

void clear_deco()
{
	int ci;
	for (ci = 0; ci < 16; ci++) {
		tissue_n2_sat[ci] = 0.0;
		tissue_he_sat[ci] = 0.0;
		tissue_tolerated_ambient_pressure[ci] = 0.0;
	}
	divetime = 0;
}

unsigned int deco_allowed_depth(double tissues_tolerance, double surface_pressure, struct dive *dive, gboolean smooth)
{
	unsigned int depth, multiples_of_3m;
	gboolean below_gradient_limit;
	double new_gradient_factor;
	double pressure_delta = tissues_tolerance - surface_pressure;
	struct dive_data mydata;

	if (pressure_delta > 0) {
		if (!smooth) {
			multiples_of_3m = (pressure_delta + DIST_FROM_3_MTR) / 0.3;
			depth = 3000 * multiples_of_3m;
		} else {
			depth = rel_mbar_to_depth(pressure_delta * 1000, dive);
		}
	} else {
		depth = 0;
	}
	mydata.pressure = surface_pressure + depth / 10000.0;
	mydata.surface = surface_pressure;

	new_gradient_factor = gradient_factor_calculation(&mydata);
	below_gradient_limit = (new_gradient_factor < actual_gradient_limit(&mydata));
	while(!below_gradient_limit)
	{
		mydata.pressure += PRESSURE_CHANGE_3M;
		new_gradient_factor = gradient_factor_calculation(&mydata);
		below_gradient_limit = (new_gradient_factor < actual_gradient_limit(&mydata));
	}

	return depth;
}
