/* space.cpp - Implementations of Space internals for Franklin {{{
 * vim: foldmethod=marker :
 * Copyright 2014-2016 Michigan Technological University
 * Copyright 2016 Bas Wijnen <wijnen@debian.org>
 * Author: Bas Wijnen <wijnen@debian.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * }}} */

#include "cdriver.h"

//#define DEBUG_PATH

#if 0
#define loaddebug debug
#else
#define loaddebug(...) do {} while(0)
#endif

#if 0
#define movedebug(...) debug(__VA_ARGS__)
#else
#define movedebug(...) do {} while (0)
#endif

// Setup. {{{
bool Space::setup_nums(int na, int nm) { // {{{
	if (na == num_axes && nm == num_motors)
		return true;
	loaddebug("new space %d %d %d %d", na, nm, num_axes, num_motors);
	int old_nm = num_motors;
	int old_na = num_axes;
	num_motors = nm;
	num_axes = na;
	if (na != old_na) {
		Axis **new_axes = new Axis *[na];
		loaddebug("new axes: %d", na);
		for (int a = 0; a < min(old_na, na); ++a)
			new_axes[a] = axis[a];
		for (int a = old_na; a < na; ++a) {
			new_axes[a] = new Axis;
			new_axes[a]->park = NAN;
			new_axes[a]->park_order = 0;
			new_axes[a]->min_pos = -INFINITY;
			new_axes[a]->max_pos = INFINITY;
			new_axes[a]->type_data = NULL;
			new_axes[a]->settings.dist[0] = NAN;
			new_axes[a]->settings.dist[1] = NAN;
			new_axes[a]->settings.main_dist = NAN;
			new_axes[a]->settings.target = NAN;
			new_axes[a]->settings.source = NAN;
			new_axes[a]->settings.current = NAN;
			new_axes[a]->history = setup_axis_history();
		}
		for (int a = na; a < old_na; ++a) {
			space_types[type].afree(this, a);
			delete[] axis[a]->history;
			delete axis[a];
		}
		delete[] axis;
		axis = new_axes;
	}
	if (nm != old_nm) {
		Motor **new_motors = new Motor *[nm];
		loaddebug("new motors: %d", nm);
		for (int m = 0; m < min(old_nm, nm); ++m)
			new_motors[m] = motor[m];
		for (int m = old_nm; m < nm; ++m) {
			new_motors[m] = new Motor;
			new_motors[m]->step_pin.init();
			new_motors[m]->dir_pin.init();
			new_motors[m]->enable_pin.init();
			new_motors[m]->limit_min_pin.init();
			new_motors[m]->limit_max_pin.init();
			new_motors[m]->steps_per_unit = 100;
			new_motors[m]->limit_v = INFINITY;
			new_motors[m]->limit_a = INFINITY;
			new_motors[m]->home_pos = NAN;
			new_motors[m]->home_order = 0;
			new_motors[m]->limit_v = INFINITY;
			new_motors[m]->limit_a = INFINITY;
			new_motors[m]->active = false;
			new_motors[m]->settings.last_v = 0;
			new_motors[m]->settings.current_pos = 0;
			new_motors[m]->settings.target_v = NAN;
			new_motors[m]->settings.target_dist = NAN;
			new_motors[m]->settings.endpos = NAN;
			new_motors[m]->history = setup_motor_history();
			ARCH_NEW_MOTOR(id, m, new_motors);
		}
		for (int m = nm; m < old_nm; ++m) {
			DATA_DELETE(id, m);
			delete[] motor[m]->history;
			delete motor[m];
		}
		delete[] motor;
		motor = new_motors;
		arch_motors_change();
	}
	return true;
} // }}}

void move_to_current() { // {{{
	if (computing_move || !motors_busy) {
		if (moving_to_current == 0)
			moving_to_current = 1;
		return;
	}
	moving_to_current = 0;
	//debug("move to current");
	settings.f0 = 0;
	settings.fmain = 1;
	settings.fp = 0;
	settings.fq = 0;
	settings.t0 = 0;
	settings.tp = 0;
	//debug("clearing %d cbs after current move for move to current", cbs_after_current_move);
	cbs_after_current_move = 0;
	current_fragment_pos = 0;
	first_fragment = current_fragment;
	computing_move = true;
	settings.cbs = 0;
	settings.hwtime = 0;
	settings.start_time = 0;
	settings.last_time = 0;
	settings.last_current_time = 0;
	for (int s = 0; s < NUM_SPACES; ++s) {
		Space &sp = spaces[s];
		sp.settings.dist[0] = 0;
		sp.settings.dist[1] = 0;
		for (int a = 0; a < sp.num_axes; ++a) {
			cpdebug(s, a, "using current %f", sp.axis[a]->settings.current);
			sp.axis[a]->settings.source = sp.axis[a]->settings.current;
			sp.axis[a]->settings.target = sp.axis[a]->settings.current;
			sp.axis[a]->settings.dist[0] = 0;
			sp.axis[a]->settings.dist[1] = 0;
			sp.axis[a]->settings.main_dist = 0;
		}
		for (int m = 0; m < sp.num_motors; ++m)
			sp.motor[m]->settings.last_v = 0;
	}
	buffer_refill();
} // }}}

void Space::load_info(int32_t &addr) { // {{{
	loaddebug("loading space %d", id);
	int t = type;
	if (t >= NUM_SPACE_TYPES)
		t = DEFAULT_TYPE;
	type = read_8(addr);
	if (type >= NUM_SPACE_TYPES || (id == 1 && type != EXTRUDER_TYPE) || (id == 2 && type != FOLLOWER_TYPE)) {
		debug("request for type %d ignored", type);
		type = t;
	}
	bool ok;
	if (t != type) {
		loaddebug("setting type to %d", type);
		space_types[t].free(this);
		if (!space_types[type].init(this)) {
			type = DEFAULT_TYPE;
			space_types[type].reset_pos(this);
			for (int a = 0; a < num_axes; ++a)
				axis[a]->settings.current = axis[a]->settings.source;
			return;	// The rest of the package is not meant for DEFAULT_TYPE, so ignore it.
		}
		ok = false;
	}
	else {
		if (computing_move || !motors_busy)
			ok = false;
		else
			ok = true;
	}
	space_types[type].load(this, t, addr);
	if (t != type) {
		space_types[type].reset_pos(this);
		loaddebug("resetting current for load space %d", id);
		for (int a = 0; a < num_axes; ++a)
			axis[a]->settings.current = axis[a]->settings.source;
	}
	if (ok)
		move_to_current();
	loaddebug("done loading space");
} // }}}

void Space::load_axis(int a, int32_t &addr) { // {{{
	loaddebug("loading axis %d", a);
	axis[a]->park = read_float(addr);
	axis[a]->park_order = read_8(addr);
	axis[a]->min_pos = read_float(addr);
	axis[a]->max_pos = read_float(addr);
} // }}}

void Space::load_motor(int m, int32_t &addr) { // {{{
	loaddebug("loading motor %d", m);
	uint16_t enable = motor[m]->enable_pin.write();
	double old_home_pos = motor[m]->home_pos;
	double old_steps_per_unit = motor[m]->steps_per_unit;
	motor[m]->step_pin.read(read_16(addr));
	motor[m]->dir_pin.read(read_16(addr));
	motor[m]->enable_pin.read(read_16(addr));
	motor[m]->limit_min_pin.read(read_16(addr));
	motor[m]->limit_max_pin.read(read_16(addr));
	motor[m]->steps_per_unit = read_float(addr);
	if (isnan(motor[m]->steps_per_unit)) {
		debug("Trying to set NaN steps per unit for motor %d %d", id, m);
		motor[m]->steps_per_unit = old_steps_per_unit;
	}
	motor[m]->home_pos = read_float(addr);
	motor[m]->limit_v = read_float(addr);
	motor[m]->limit_a = read_float(addr);
	motor[m]->home_order = read_8(addr);
	arch_motors_change();
	SET_OUTPUT(motor[m]->enable_pin);
	if (enable != motor[m]->enable_pin.write()) {
		if (motors_busy)
			SET(motor[m]->enable_pin);
		else {
			RESET(motor[m]->enable_pin);
		}
	}
	RESET(motor[m]->step_pin);
	SET_INPUT(motor[m]->limit_min_pin);
	SET_INPUT(motor[m]->limit_max_pin);
	bool must_move = false;
	if (!isnan(motor[m]->home_pos)) {
		// Axes with a limit switch.
		if (motors_busy && (old_home_pos != motor[m]->home_pos || old_steps_per_unit != motor[m]->steps_per_unit) && !isnan(old_home_pos)) {
			double ohp = old_home_pos * old_steps_per_unit;
			double hp = motor[m]->home_pos * motor[m]->steps_per_unit;
			double diff = hp - ohp;
			motor[m]->settings.current_pos += diff;
			//debug("load motor %d %d new home %f add %f", id, m, motor[m]->home_pos, diff);
			arch_addpos(id, m, diff);
			must_move = true;
		}
	}
	else {
		// Axes without a limit switch, including extruders.
		if (motors_busy && old_steps_per_unit != motor[m]->steps_per_unit) {
			debug("load motor %d %d new steps no home", id, m);
			double oldpos = motor[m]->settings.current_pos;
			double pos = oldpos / old_steps_per_unit;
			motor[m]->settings.current_pos = pos * motor[m]->steps_per_unit;
			arch_addpos(id, m, motor[m]->settings.current_pos - oldpos);
			// Adjust current_pos in all history.
			for (int h = 0; h < FRAGMENTS_PER_BUFFER; ++h) {
				oldpos = motor[m]->history[h].current_pos;
				pos = oldpos / old_steps_per_unit;
				motor[m]->history[h].current_pos = pos * motor[m]->steps_per_unit;
			}
		}
	}
	if (must_move)
		move_to_current();
} // }}}

void Space::save_info(int32_t &addr) { // {{{
	write_8(addr, type);
	space_types[type].save(this, addr);
} // }}}

void Space::save_axis(int a, int32_t &addr) { // {{{
	write_float(addr, axis[a]->park);
	write_8(addr, axis[a]->park_order);
	write_float(addr, axis[a]->min_pos);
	write_float(addr, axis[a]->max_pos);
} // }}}

void Space::save_motor(int m, int32_t &addr) { // {{{
	write_16(addr, motor[m]->step_pin.write());
	write_16(addr, motor[m]->dir_pin.write());
	write_16(addr, motor[m]->enable_pin.write());
	write_16(addr, motor[m]->limit_min_pin.write());
	write_16(addr, motor[m]->limit_max_pin.write());
	write_float(addr, motor[m]->steps_per_unit);
	write_float(addr, motor[m]->home_pos);
	write_float(addr, motor[m]->limit_v);
	write_float(addr, motor[m]->limit_a);
	write_8(addr, motor[m]->home_order);
} // }}}

void Space::init(int space_id) { // {{{
	type = space_id == 0 ? DEFAULT_TYPE : space_id == 1 ? EXTRUDER_TYPE : FOLLOWER_TYPE;
	id = space_id;
	type_data = NULL;
	num_axes = 0;
	num_motors = 0;
	motor = NULL;
	axis = NULL;
	history = NULL;
	space_types[type].init(this);
} // }}}

void Space::cancel_update() { // {{{
	// setup_nums failed; restore system to a usable state.
	type = DEFAULT_TYPE;
	int n = min(num_axes, num_motors);
	if (!setup_nums(n, n)) {
		debug("Failed to free memory; removing all motors and axes to make sure it works");
		if (!setup_nums(0, 0))
			debug("You're in trouble; this shouldn't be possible");
	}
} // }}}
// }}}

// Movement handling. {{{
static void check_distance(int sp, int mt, Motor *mtr, double distance, double dt, double &factor) { // {{{
	if (dt == 0) {
		factor = 0;
		return;
	}
	if (isnan(distance) || distance == 0) {
		mtr->settings.target_dist = 0;
		//mtr->last_v = 0;
		return;
	}
	//debug("cd %f %f", distance, dt);
	mtr->settings.target_dist = distance;
	mtr->settings.target_v = distance / dt;
	double v = fabs(mtr->settings.target_v);
	int s = (mtr->settings.target_v < 0 ? -1 : 1);
	// When turning around, ignore limits (they shouldn't have been violated anyway).
	if (mtr->settings.last_v * s < 0) {
		//debug("!");
		mtr->settings.last_v = 0;
	}
	// Limit v.
	if (v > mtr->limit_v) {
		//debug("v %f limit %f", v, mtr->limit_v);
		distance = (s * mtr->limit_v) * dt;
		v = fabs(distance / dt);
	}
	//debug("cd2 %f %f", distance, dt);
	// Limit a+.
	double limit_dv = mtr->limit_a * dt;
	if (v - mtr->settings.last_v * s > limit_dv) {
		//debug("a+ target v %f limit dv %f last v %f s %d", mtr->settings.target_v, limit_dv, mtr->settings.last_v, s);
		distance = (limit_dv * s + mtr->settings.last_v) * dt;
		v = fabs(distance / dt);
	}
	//debug("cd3 %f %f", distance, dt);
	// Limit a-.
	// Distance to travel until end of segment or connection.
	double max_dist = (mtr->settings.endpos - mtr->settings.current_pos / mtr->steps_per_unit) * s;
	// Find distance traveled when slowing down at maximum a.
	// x = 1/2 at²
	// t = sqrt(2x/a)
	// v = at
	// v² = 2a²x/a = 2ax
	// x = v²/2a
	double limit_dist = v * v / 2 / mtr->limit_a;
	//debug("max %f limit %f v %f a %f", max_dist, limit_dist, v, mtr->limit_a);
	if (max_dist > 0 && limit_dist > max_dist) {
		//debug("a- endpos %f limit a %f limit dist %f max dist %f v %f distance %f current pos %f s %d dt %f", mtr->settings.endpos, mtr->limit_a, limit_dist, max_dist, v, distance, mtr->settings.current_pos, s, dt);
		v = sqrt(max_dist * 2 * mtr->limit_a);
		distance = s * v * dt;
	}
	//debug("cd4 %f %f", distance, dt); */
	int steps = arch_round_pos(sp, mt, mtr->settings.current_pos + distance * mtr->steps_per_unit) - round(mtr->settings.current_pos);
	int targetsteps = steps;
	//cpdebug(s, m, "cf %d value %d", current_fragment, value);
	if (settings.probing && steps)
		steps = s;
	else {
		// Maximum depends on number of subfragments.  Be conservative and use 0x7e per subfragment; assume 128 subfragments (largest power of 2 smaller than 10000/75)
		// TODO: 10000 and 75 should follow the actual values for step_time in cdriver and TIME_PER_ISR in firmware.
		int max = 0x1e << 7;
		if (abs(steps) > max) {
			debug("overflow %d from cp %f dist %f steps/mm %f dt %f s %d max %d", steps, mtr->settings.current_pos, distance, mtr->steps_per_unit, dt, s, max);
			steps = max * s;
		}
	}
	if (abs(steps) < abs(targetsteps)) {
		distance = (arch_round_pos(sp, mt, mtr->settings.current_pos) + steps + s * .5 - mtr->settings.current_pos) / mtr->steps_per_unit;
		v = fabs(distance / dt);
	}
	//debug("=============");
	double f = distance / mtr->settings.target_dist;
	//debug("checked %f %f %f %f", mtr->settings.target_dist, distance, factor, f);
	if (f < factor)
		factor = f;
} // }}}

static void move_axes(Space *s, int32_t current_time, double &factor) { // {{{
	double motors_target[s->num_motors];
	bool ok = true;
	space_types[s->type].xyz2motors(s, motors_target);
	// Try again if it didn't work; it should have moved target to a better location.
	if (!ok) {
		space_types[s->type].xyz2motors(s, motors_target);
		movedebug("retried move");
	}
	//movedebug("ok %d", ok);
	for (int m = 0; m < s->num_motors; ++m) {
		//if (s->id == 0 && m == 0)
			//debug("check move %d %d target %f current %f", s->id, m, motors_target[m], s->motor[m]->settings.current_pos / s->motor[m]->steps_per_unit);
		double distance = motors_target[m] - s->motor[m]->settings.current_pos / s->motor[m]->steps_per_unit;
		check_distance(s->id, m, s->motor[m], distance, (current_time - settings.last_time) / 1e6, factor);
	}
} // }}}

static bool do_steps(double &factor, int32_t current_time) { // {{{
	//debug("steps");
	if (factor <= 0) {
		movedebug("end move");
		// Callers expect a new value to be inserted in the buffers.
		for (int s = 0; s < NUM_SPACES; ++s) {
			if (!settings.single && s == 2)
				continue;
			Space &sp = spaces[s];
			for (int m = 0; m < sp.num_motors; ++m)
				DATA_SET(s, m, 0);
		}
		current_fragment_pos += 1;
		return false;
	}
	// Check if more steps should be done, to detect end of move.
	bool have_steps = false;
	for (int s = 0; !have_steps && s < NUM_SPACES; ++s) {
		if (!settings.single && s == 2)
			continue;
		Space &sp = spaces[s];
		for (int m = 0; m < sp.num_motors; ++m) {
			Motor &mtr = *sp.motor[m];
			// Check if there are steps to be done; ignore factor.
			double num = (mtr.settings.current_pos / mtr.steps_per_unit + mtr.settings.target_dist);
			//debug("num: %f ; current: %f", num, mtr.settings.current_pos);
			if (arch_round_pos(s, m, mtr.settings.current_pos) != arch_round_pos(s, m, num * mtr.steps_per_unit)) {
				//debug("have steps %f %f %f", mtr.settings.current_pos, mtr.settings.target_dist, factor);
				have_steps = true;
				break;
			}
			//debug("no steps yet %d %d %f %f %f", s, m, mtr.settings.current_pos, mtr.settings.target_dist, factor);
		}
	}
	for (int s = 0; s < NUM_SPACES; ++s) {
		if (!settings.single && s == 2)
			continue;
		Space &sp = spaces[s];
		for (int a = 0; a < sp.num_axes; ++a) {
			if (!isnan(sp.axis[a]->settings.target)) {
				//debug("add %d %d %f -> %f", s, a, (sp.axis[a]->settings.target - sp.axis[a]->settings.current) * factor, sp.axis[a]->settings.current);
				sp.axis[a]->settings.current += (sp.axis[a]->settings.target - sp.axis[a]->settings.current) * factor;
			}
		}
	}
	if (factor < 1) {
		// Recalculate steps; ignore resulting factor.
		double dummy_factor = 1;
		//debug("redo steps %d", current_fragment);
		for (int s = 0; s < NUM_SPACES; ++s) {
			if (!settings.single && s == 2)
				continue;
			Space &sp = spaces[s];
			for (int a = 0; a < sp.num_axes; ++a) {
				if (!isnan(sp.axis[a]->settings.target))
					sp.axis[a]->settings.target = sp.axis[a]->settings.current;
			}
			move_axes(&sp, settings.last_time, dummy_factor);
		}
	}
	//debug("do steps %f", factor);
	int32_t the_last_time = settings.last_current_time;
	settings.last_current_time = current_time;
	// Adjust start time if factor < 1.
	if (factor < 1) {
		settings.start_time += (current_time - the_last_time) * ((1 - factor) * .99);
		movedebug("correct: %f %d", factor, int(settings.start_time));
	}
	else
		movedebug("no correct: %f %d", factor, int(settings.start_time));
	settings.last_time = current_time;
#ifdef DEBUG_PATH
	fprintf(stderr, "%d", current_time);
	for (int a = 0; a < spaces[0].num_axes; ++a) {
		if (isnan(spaces[0].axis[a]->settings.target))
			fprintf(stderr, "\t%f", spaces[0].axis[a]->settings.source);
		else
			fprintf(stderr, "\t%f", spaces[0].axis[a]->settings.target);
	}
	fprintf(stderr, "\n");
#endif
	// Move the motors.
	//debug("start move");
	have_steps = false;
	for (int s = 0; s < NUM_SPACES; ++s) {
		if (!settings.single && s == 2)
			continue;
		Space &sp = spaces[s];
		for (int m = 0; m < sp.num_motors; ++m) {
			Motor &mtr = *sp.motor[m];
			if (isnan(mtr.settings.target_dist) || mtr.settings.target_dist == 0) {
				//debug("no %d %d", s, m);
				//if (mtr.target_v == 0)
					//mtr.last_v = 0;
				continue;
			}
			double target = mtr.settings.current_pos / mtr.steps_per_unit + mtr.settings.target_dist * factor;
			cpdebug(s, m, "ccp3 stopping %d target %f lastv %f spm %f tdist %f factor %f frag %d", stopping, target, mtr.settings.last_v, mtr.steps_per_unit, mtr.settings.target_dist, factor, current_fragment);
			double new_cp = target * mtr.steps_per_unit;
			if (arch_round_pos(s, m, mtr.settings.current_pos) != arch_round_pos(s, m, new_cp)) {
				have_steps = true;
				if (!mtr.active) {
					mtr.active = true;
					num_active_motors += 1;
				}
				int diff = arch_round_pos(s, m, new_cp) - arch_round_pos(s, m, mtr.settings.current_pos);
				movedebug("sending %d %d steps %d", s, m, diff);
				DATA_SET(s, m, diff);
			}
			//debug("new cp: %d %d %f %d", s, m, new_cp, current_fragment_pos);
			if (!settings.single) {
				for (int mm = 0; mm < spaces[2].num_motors; ++mm) {
					int fm = space_types[spaces[2].type].follow(&spaces[2], mm);
					if (fm < 0)
						continue;
					int fs = fm >> 8;
					fm &= 0x7f;
					if (fs != s || fm != m || (fs == 2 && fm >= mm))
						continue;
					//debug("follow %d %d %d %d %d %f %f", s, m, fs, fm, mm, new_cp, mtr.settings.current_pos);
					spaces[2].motor[mm]->settings.current_pos += new_cp - mtr.settings.current_pos;
				}
			}
			mtr.settings.current_pos = new_cp;
			//cpdebug(s, m, "cp three %f", target);
			mtr.settings.last_v = mtr.settings.target_v * factor;
		}
	}
	current_fragment_pos += 1;
	//debug("have steps: %d", have_steps);
	return have_steps;
} // }}}

void make_target(Space &sp, double f, bool next) { // {{{
	int a = 0;
	if (sp.settings.arc[next]) {
		// Convert time-fraction into angle-fraction.
		f = (2 * sp.settings.radius[next][0] * f) / (sp.settings.radius[next][0] + sp.settings.radius[next][1] - (sp.settings.radius[next][1] - sp.settings.radius[next][0]) * f);
		double angle = sp.settings.angle[next] * f;
		double radius = sp.settings.radius[next][0] + (sp.settings.radius[next][1] - sp.settings.radius[next][0]) * f;
		double helix = sp.settings.helix[next] * f;
		double cosa = cos(angle);
		double sina = sin(angle);
		//debug("e1 %f %f %f e2 %f %f %f", sp.settings.e1[next][0], sp.settings.e1[next][1], sp.settings.e1[next][2], sp.settings.e2[next][0], sp.settings.e2[next][1], sp.settings.e2[next][2]);
		for (int i = 0; i < min(3, sp.num_axes); ++i)
			sp.axis[i]->settings.target += radius * cosa * sp.settings.e1[next][i] + radius * sina * sp.settings.e2[next][i] - sp.settings.offset[next][i] + helix * sp.settings.normal[next][i];
		a = 3;
		// Fall through.
	}
	for (; a < sp.num_axes; ++a) {
		sp.axis[a]->settings.target += sp.axis[a]->settings.dist[next] * f;
		movedebug("do %d %d %f %f", sp.id, a, sp.axis[a]->settings.dist[0], sp.axis[a]->settings.target);
	}
} // }}}

static void handle_motors(unsigned long long current_time) { // {{{
	// Check for move.
	if (!computing_move) {
		movedebug("handle motors not moving");
		return;
	}
	movedebug("handling %d %d", computing_move, cbs_after_current_move);
	double factor = 1;
	double t = (current_time - settings.start_time) / 1e6;
	if (t >= settings.t0 + settings.tp) {	// Finish this move and prepare next. {{{
		movedebug("finishing %f %f %f %ld %ld", t, settings.t0, settings.tp, long(current_time), long(settings.start_time));
		//debug("finish steps");
		for (int s = 0; s < NUM_SPACES; ++s) {
			if (!settings.single && s == 2)
				continue;
			Space &sp = spaces[s];
			if (!isnan(sp.settings.dist[0])) {
				for (int a = 0; a < sp.num_axes; ++a) {
					if (!isnan(sp.axis[a]->settings.dist[0])) {
						sp.axis[a]->settings.source += sp.axis[a]->settings.dist[0];
						sp.axis[a]->settings.dist[0] = NAN;
						//debug("new source %d %f", a, sp.axis[a]->settings.source);
					}
				}
				sp.settings.dist[0] = NAN;
			}
			for (int a = 0; a < sp.num_axes; ++a)
				sp.axis[a]->settings.target = sp.axis[a]->settings.source;
			move_axes(&sp, current_time, factor);
			//debug("f %f", factor);
		}
		//debug("f2 %f %ld %ld", factor, settings.last_time, current_time);
		bool did_steps = do_steps(factor, current_time);
		//debug("f3 %f", factor);
		// Start time may have changed; recalculate t.
		t = (current_time - settings.start_time) / 1e6;
		if (t / (settings.t0 + settings.tp) >= done_factor) {
			movedebug("Done with this move");
			int had_cbs = cbs_after_current_move;
			//debug("clearing %d cbs after current move for later inserting into history", cbs_after_current_move);
			cbs_after_current_move = 0;
			run_file_fill_queue();
			if (settings.queue_start != settings.queue_end || settings.queue_full) {
				movedebug("queue is not empty");
				had_cbs += next_move();
				if (!aborting && had_cbs > 0) {
					int fragment;
					if (num_active_motors == 0)
						fragment = (current_fragment + FRAGMENTS_PER_BUFFER - 1) % FRAGMENTS_PER_BUFFER;
					else
						fragment = current_fragment;
					//debug("adding %d cbs to fragment %d", had_cbs, fragment);
					history[fragment].cbs += had_cbs;
				}
				return;
			}
			else
				movedebug("queue is empty");
			cbs_after_current_move += had_cbs;
			//debug("adding %d to cbs after current move making it %d", had_cbs, cbs_after_current_move);
			if (factor == 1) {
				//debug("queue done");
				if (!did_steps) {
					movedebug("really done move");
					computing_move = false;
					// Cut off final sample, which was no steps anyway.
					current_fragment_pos -= 1;
				}
				for (int s = 0; s < NUM_SPACES; ++s) {
					Space &sp = spaces[s];
					for (int m = 0; m < sp.num_motors; ++m)
						sp.motor[m]->settings.last_v = 0;
				}
				if (cbs_after_current_move > 0) {
					if (!aborting) {
						int fragment;
						if (num_active_motors == 0)
							if (running_fragment == current_fragment) {
								//debug("sending movecbs immediately");
								send_host(CMD_MOVECB, cbs_after_current_move);
								cbs_after_current_move = 0;
								return;
							}
							else
								fragment = (current_fragment + FRAGMENTS_PER_BUFFER - 1) % FRAGMENTS_PER_BUFFER;
						else
							fragment = current_fragment;
						//debug("adding %d cbs to final fragment %d", cbs_after_current_move, fragment);
						history[fragment].cbs += cbs_after_current_move;
					}
					//debug("clearing %d cbs after current move in final", cbs_after_current_move);
					cbs_after_current_move = 0;
				}
			}
			//else
				//debug("queue not done yet: %f", factor);
		}
		return;
	} // }}}
	if (t < settings.t0) {	// Main part. {{{
		double t_fraction = t / settings.t0;
		double current_f = (settings.f1 * (2 - t_fraction) + settings.f2 * t_fraction) * t_fraction;
		movedebug("main t %f t0 %f tp %f tfrac %f f1 %f f2 %f cf %f", t, settings.t0, settings.tp, t_fraction, settings.f1, settings.f2, current_f);
		//debug("main steps");
		for (int s = 0; s < NUM_SPACES; ++s) {
			if (!settings.single && s == 2)
				continue;
			Space &sp = spaces[s];
			for (int a = 0; a < sp.num_axes; ++a) {
				if (isnan(sp.axis[a]->settings.dist[0])) {
					sp.axis[a]->settings.target = NAN;
					continue;
				}
				sp.axis[a]->settings.target = sp.axis[a]->settings.source;
			}
			make_target(sp, current_f, false);
			move_axes(&sp, current_time, factor);
		}
	} // }}}
	else {	// Connector part. {{{
		movedebug("connector %f %f %f", t, settings.t0, settings.tp);
		double tc = t - settings.t0;
		double t_fraction = tc / settings.tp;
		double current_f2 = settings.fp * (2 - t_fraction) * t_fraction;
		double current_f3 = settings.fq * t_fraction * t_fraction;
		//debug("connect steps");
		for (int s = 0; s < NUM_SPACES; ++s) {
			if (!settings.single && s == 2)
				continue;
			Space &sp = spaces[s];
			for (int a = 0; a < sp.num_axes; ++a) {
				if (isnan(sp.axis[a]->settings.dist[0]) && isnan(sp.axis[a]->settings.dist[1])) {
					sp.axis[a]->settings.target = NAN;
					continue;
				}
				sp.axis[a]->settings.target = sp.axis[a]->settings.source;
			}
			make_target(sp, (1 - settings.fp) + current_f2, false);
			make_target(sp, current_f3, true);
			move_axes(&sp, current_time, factor);
		}
	} // }}}
	do_steps(factor, current_time);
} // }}}

void store_settings() { // {{{
	current_fragment_pos = 0;
	num_active_motors = 0;
	if (FRAGMENTS_PER_BUFFER == 0)
		return;
	history[current_fragment].t0 = settings.t0;
	history[current_fragment].tp = settings.tp;
	history[current_fragment].f0 = settings.f0;
	history[current_fragment].f1 = settings.f1;
	history[current_fragment].f2 = settings.f2;
	history[current_fragment].fp = settings.fp;
	history[current_fragment].fq = settings.fq;
	history[current_fragment].fmain = settings.fmain;
	history[current_fragment].cbs = 0;
	history[current_fragment].hwtime = settings.hwtime;
	history[current_fragment].start_time = settings.start_time;
	history[current_fragment].last_time = settings.last_time;
	history[current_fragment].last_current_time = settings.last_current_time;
	history[current_fragment].queue_start = settings.queue_start;
	history[current_fragment].queue_end = settings.queue_end;
	history[current_fragment].queue_full = settings.queue_full;
	history[current_fragment].run_file_current = settings.run_file_current;
	history[current_fragment].run_time = settings.run_time;
	history[current_fragment].run_dist = settings.run_dist;
	for (int s = 0; s < NUM_SPACES; ++s) {
		Space &sp = spaces[s];
		sp.history[current_fragment].dist[0] = sp.settings.dist[0];
		sp.history[current_fragment].dist[1] = sp.settings.dist[1];
		for (int i = 0; i < 2; ++i) {
			sp.history[current_fragment].arc[i] = sp.settings.arc[i];
			sp.history[current_fragment].angle[i] = sp.settings.angle[i];
			sp.history[current_fragment].helix[i] = sp.settings.helix[i];
			for (int t = 0; t < 2; ++t)
				sp.history[current_fragment].radius[i][t] = sp.settings.radius[i][t];
			for (int t = 0; t < 3; ++t) {
				sp.history[current_fragment].offset[i][t] = sp.settings.offset[i][t];
				sp.history[current_fragment].e1[i][t] = sp.settings.e1[i][t];
				sp.history[current_fragment].e2[i][t] = sp.settings.e2[i][t];
				sp.history[current_fragment].normal[i][t] = sp.settings.normal[i][t];
			}
		}
		for (int m = 0; m < sp.num_motors; ++m) {
			sp.motor[m]->active = false;
			DATA_CLEAR(s, m);
			sp.motor[m]->history[current_fragment].last_v = sp.motor[m]->settings.last_v;
			sp.motor[m]->history[current_fragment].target_v = sp.motor[m]->settings.target_v;
			sp.motor[m]->history[current_fragment].target_dist = sp.motor[m]->settings.target_dist;
			sp.motor[m]->history[current_fragment].current_pos = sp.motor[m]->settings.current_pos;
			sp.motor[m]->history[current_fragment].endpos = sp.motor[m]->settings.endpos;
			cpdebug(s, m, "store");
		}
		for (int a = 0; a < sp.num_axes; ++a) {
			sp.axis[a]->history[current_fragment].dist[0] = sp.axis[a]->settings.dist[0];
			sp.axis[a]->history[current_fragment].dist[1] = sp.axis[a]->settings.dist[1];
			sp.axis[a]->history[current_fragment].main_dist = sp.axis[a]->settings.main_dist;
			sp.axis[a]->history[current_fragment].target = sp.axis[a]->settings.target;
			sp.axis[a]->history[current_fragment].source = sp.axis[a]->settings.source;
			sp.axis[a]->history[current_fragment].current = sp.axis[a]->settings.current;
			sp.axis[a]->history[current_fragment].endpos[0] = sp.axis[a]->settings.endpos[0];
			sp.axis[a]->history[current_fragment].endpos[1] = sp.axis[a]->settings.endpos[1];
		}
	}
} // }}}

void restore_settings() { // {{{
	current_fragment_pos = 0;
	num_active_motors = 0;
	if (FRAGMENTS_PER_BUFFER == 0)
		return;
	settings.t0 = history[current_fragment].t0;
	settings.tp = history[current_fragment].tp;
	settings.f0 = history[current_fragment].f0;
	settings.f1 = history[current_fragment].f1;
	settings.f2 = history[current_fragment].f2;
	settings.fp = history[current_fragment].fp;
	settings.fq = history[current_fragment].fq;
	settings.fmain = history[current_fragment].fmain;
	history[current_fragment].cbs = 0;
	settings.hwtime = history[current_fragment].hwtime;
	settings.start_time = history[current_fragment].start_time;
	settings.last_time = history[current_fragment].last_time;
	settings.last_current_time = history[current_fragment].last_current_time;
	settings.queue_start = history[current_fragment].queue_start;
	settings.queue_end = history[current_fragment].queue_end;
	settings.queue_full = history[current_fragment].queue_full;
	settings.run_file_current = history[current_fragment].run_file_current;
	settings.run_time = history[current_fragment].run_time;
	settings.run_dist = history[current_fragment].run_dist;
	for (int s = 0; s < NUM_SPACES; ++s) {
		Space &sp = spaces[s];
		sp.settings.dist[0] = sp.history[current_fragment].dist[0];
		sp.settings.dist[1] = sp.history[current_fragment].dist[1];
		for (int i = 0; i < 2; ++i) {
			sp.settings.arc[i] = sp.history[current_fragment].arc[i];
			sp.settings.angle[i] = sp.history[current_fragment].angle[i];
			sp.settings.helix[i] = sp.history[current_fragment].helix[i];
			for (int t = 0; t < 2; ++t)
				sp.settings.radius[i][t] = sp.history[current_fragment].radius[i][t];
			for (int t = 0; t < 3; ++t) {
				sp.settings.offset[i][t] = sp.history[current_fragment].offset[i][t];
				sp.settings.e1[i][t] = sp.history[current_fragment].e1[i][t];
				sp.settings.e2[i][t] = sp.history[current_fragment].e2[i][t];
				sp.settings.normal[i][t] = sp.history[current_fragment].normal[i][t];
			}
		}
		for (int m = 0; m < sp.num_motors; ++m) {
			sp.motor[m]->active = false;
			DATA_CLEAR(s, m);
			sp.motor[m]->settings.last_v = sp.motor[m]->history[current_fragment].last_v;
			sp.motor[m]->settings.target_v = sp.motor[m]->history[current_fragment].target_v;
			sp.motor[m]->settings.target_dist = sp.motor[m]->history[current_fragment].target_dist;
			sp.motor[m]->settings.current_pos = sp.motor[m]->history[current_fragment].current_pos;
			sp.motor[m]->settings.endpos = sp.motor[m]->history[current_fragment].endpos;
			cpdebug(s, m, "restore");
		}
		for (int a = 0; a < sp.num_axes; ++a) {
			sp.axis[a]->settings.dist[0] = sp.axis[a]->history[current_fragment].dist[0];
			sp.axis[a]->settings.dist[1] = sp.axis[a]->history[current_fragment].dist[1];
			sp.axis[a]->settings.main_dist = sp.axis[a]->history[current_fragment].main_dist;
			sp.axis[a]->settings.target = sp.axis[a]->history[current_fragment].target;
			sp.axis[a]->settings.source = sp.axis[a]->history[current_fragment].source;
			sp.axis[a]->settings.current = sp.axis[a]->history[current_fragment].current;
			sp.axis[a]->settings.endpos[0] = sp.axis[a]->history[current_fragment].endpos[0];
			sp.axis[a]->settings.endpos[1] = sp.axis[a]->history[current_fragment].endpos[1];
		}
	}
} // }}}

void send_fragment() { // {{{
	if (host_block) {
		current_fragment_pos = 0;
		return;
	}
	if (current_fragment_pos <= 0 || stopping || sending_fragment) {
		debug("no send fragment %d %d %d", current_fragment_pos, stopping, sending_fragment);
		return;
	}
	if (num_active_motors == 0) {
		if (current_fragment_pos < 2) {
			// TODO: find out why this is attempted and avoid it.
			debug("not sending short fragment for 0 motors; %d %d", current_fragment, running_fragment);
			if (history[current_fragment].cbs) {
				if (settings.queue_start == settings.queue_end && !settings.queue_full) {
					// Send cbs immediately.
					if (!host_block) {
						send_host(CMD_MOVECB, history[current_fragment].cbs);  
						history[current_fragment].cbs = 0;
					}
				}
			}
			current_fragment_pos = 0;
			return;
		}
		else
			debug("sending fragment for 0 motors at position %d", current_fragment_pos);
		//abort();
	}
	//debug("sending %d prevcbs %d", current_fragment, history[(current_fragment + FRAGMENTS_PER_BUFFER - 1) % FRAGMENTS_PER_BUFFER].cbs);
	if (arch_send_fragment()) {
		current_fragment = (current_fragment + 1) % FRAGMENTS_PER_BUFFER;
		//debug("current_fragment = (current_fragment + 1) %% FRAGMENTS_PER_BUFFER; %d", current_fragment);
		//debug("current send -> %x", current_fragment);
		store_settings();
		if ((current_fragment - running_fragment + FRAGMENTS_PER_BUFFER) % FRAGMENTS_PER_BUFFER >= MIN_BUFFER_FILL && !stopping) {
			arch_start_move(0);
		}
	}
} // }}}

void apply_tick() { // {{{
	settings.hwtime += hwtime_step;
	if (current_fragment_pos < SAMPLES_PER_FRAGMENT)
		handle_motors(settings.hwtime);
	//if (spaces[0].num_axes >= 2)
		//debug("move z %d %d %f %f %f", current_fragment, current_fragment_pos, spaces[0].axis[2]->settings.current, spaces[0].motor[0]->settings.current_pos, spaces[0].motor[0]->settings.current_pos + avr_pos_offset[0]);
} // }}}

void buffer_refill() { // {{{
	//debug("refill");
	if (preparing || FRAGMENTS_PER_BUFFER == 0) {
		//debug("no refill because prepare");
		return;
	}
	if (moving_to_current == 2)
		move_to_current();
	if (!computing_move || refilling || stopping || discard_pending || discarding) {
		//debug("refill block %d %d %d %d %d", computing_move, refilling, stopping, discard_pending, discarding);
		return;
	}
	refilling = true;
	// send_fragment in the previous refill may have failed; try it again.
	if (current_fragment_pos > 0)
		send_fragment();
	//debug("refill start %d %d %d", running_fragment, current_fragment, sending_fragment);
	// Keep one free fragment, because we want to be able to rewind and use the buffer before the one currently active.
	while (computing_move && !stopping && !discard_pending && !discarding && (running_fragment - 1 - current_fragment + FRAGMENTS_PER_BUFFER) % FRAGMENTS_PER_BUFFER > 4 && !sending_fragment) {
		//debug("refill %d %d %f", current_fragment, current_fragment_pos, spaces[0].motor[0]->settings.current_pos);
		// fill fragment until full.
		apply_tick();
		//debug("refill2 %d %f", current_fragment, spaces[0].motor[0]->settings.current_pos);
		if (current_fragment_pos >= SAMPLES_PER_FRAGMENT) {
			//debug("fragment full %d %d %d", computing_move, current_fragment_pos, BYTES_PER_FRAGMENT);
			send_fragment();
		}
		// Check for commands from host; in case of many short buffers, this loop may not end in a reasonable time.
		//serial(0);
	}
	if (stopping || discard_pending) {
		//debug("aborting refill for stopping");
		refilling = false;
		return;
	}
	if (!computing_move && current_fragment_pos > 0) {
		//debug("finalize");
		send_fragment();
	}
	refilling = false;
	arch_start_move(0);
} // }}}
// }}}
