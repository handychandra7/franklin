#include "cdriver.h"

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

bool Space::setup_nums(uint8_t na, uint8_t nm) {
	if (na == num_axes && nm == num_motors)
		return true;
	loaddebug("new space %d %d %d %d", na, nm, num_axes, num_motors);
	uint8_t old_nm = num_motors;
	uint8_t old_na = num_axes;
	num_motors = nm;
	num_axes = na;
	if (na != old_na) {
		Axis **new_axes = new Axis *[na];
		loaddebug("new axes: %d", na);
		for (uint8_t a = 0; a < min(old_na, na); ++a)
			new_axes[a] = axis[a];
		for (uint8_t a = old_na; a < na; ++a) {
			new_axes[a] = new Axis;
			new_axes[a]->source = NAN;
			new_axes[a]->current = NAN;
			new_axes[a]->offset = 0;
			new_axes[a]->park = NAN;
			new_axes[a]->park_order = 0;
			new_axes[a]->max_v = INFINITY;
			new_axes[a]->min = -INFINITY;
			new_axes[a]->max = INFINITY;
			new_axes[a]->settings = new Axis_History[FRAGMENTS_PER_BUFFER];
			for (int f = 0; f < FRAGMENTS_PER_BUFFER; ++f) {
				new_axes[a]->settings[f].dist = NAN;
				new_axes[a]->settings[f].next_dist = NAN;
				new_axes[a]->settings[f].main_dist = NAN;
				new_axes[a]->settings[f].target = NAN;
			}
		}
		for (uint8_t a = na; a < old_na; ++a) {
			delete[] axis[a]->settings;
			delete axis[a];
		}
		delete[] axis;
		axis = new_axes;
	}
	if (nm != old_nm) {
		Motor **new_motors = new Motor *[nm];
		loaddebug("new motors: %d", nm);
		for (uint8_t m = 0; m < min(old_nm, nm); ++m)
			new_motors[m] = motor[m];
		for (uint8_t m = old_nm; m < nm; ++m) {
			new_motors[m] = new Motor;
			new_motors[m]->step_pin.init();
			new_motors[m]->dir_pin.init();
			new_motors[m]->enable_pin.init();
			new_motors[m]->limit_min_pin.init();
			new_motors[m]->limit_max_pin.init();
			new_motors[m]->sense_pin.init();
			new_motors[m]->sense_state = 0;
			new_motors[m]->sense_pos = NAN;
			new_motors[m]->steps_per_m = NAN;
			new_motors[m]->max_steps = 1;
			new_motors[m]->limit_v = INFINITY;
			new_motors[m]->limit_a = INFINITY;
			new_motors[m]->home_pos = NAN;
			new_motors[m]->home_order = 0;
			new_motors[m]->limit_v = INFINITY;
			new_motors[m]->limit_a = INFINITY;
#ifdef HAVE_AUDIO
			new_motors[m]->audio_flags = 0;
#endif
			new_motors[m]->settings = new Motor_History[FRAGMENTS_PER_BUFFER];
			for (int f = 0; f < FRAGMENTS_PER_BUFFER; ++f) {
				new_motors[m]->settings[f].dir = 0;
				new_motors[m]->settings[f].data = new char[BYTES_PER_FRAGMENT];
				memset(new_motors[m]->settings[f].data, 0, BYTES_PER_FRAGMENT);
				new_motors[m]->settings[f].last_v = 0;
				new_motors[m]->settings[f].current_pos = 0;
				new_motors[m]->settings[f].hwcurrent_pos = 0;
				new_motors[m]->settings[f].last_v = NAN;
				new_motors[m]->settings[f].target_v = NAN;
				new_motors[m]->settings[f].target_dist = NAN;
				new_motors[m]->settings[f].endpos = NAN;
			}
		}
		for (uint8_t m = nm; m < old_nm; ++m) {
			for (int f = 0; f < FRAGMENTS_PER_BUFFER; ++f)
				delete[] motor[m]->settings[f].data;
			delete[] motor[m]->settings;
			delete motor[m];
		}
		delete[] motor;
		motor = new_motors;
		arch_motors_change();
	}
	return true;
}

static void move_to_current(Space *s) {
	if (moving || !motors_busy)
		return;
	settings[current_fragment].f0 = 0;
	settings[current_fragment].fmain = 1;
	settings[current_fragment].fp = 0;
	settings[current_fragment].fq = 0;
	settings[current_fragment].t0 = 0;
	settings[current_fragment].tp = 0;
	cbs_after_current_move = 0;
	current_fragment_pos = 0;
	moving = true;
	settings[current_fragment].cbs = 0;
	settings[current_fragment].hwtime = 0;
	settings[current_fragment].start_time = 0;
	settings[current_fragment].last_time = 0;
	settings[current_fragment].last_current_time = 0;
	//debug("move to current");
	for (uint8_t i = 0; i < min(s->num_axes, s->num_motors); ++i) {
		s->axis[i]->settings[current_fragment].dist = 0;
		s->axis[i]->settings[current_fragment].next_dist = 0;
		s->axis[i]->settings[current_fragment].main_dist = 0;
		s->motor[i]->settings[current_fragment].last_v = 0;
	}
	buffer_refill();
}

void Space::load_info(int32_t &addr)
{
	loaddebug("loading space");
	uint8_t t = type;
	if (t >= NUM_SPACE_TYPES || !have_type[t])
		t = DEFAULT_TYPE;
	type = read_8(addr);
	if (type >= NUM_SPACE_TYPES || !have_type[type]) {
		debug("request for type %d ignored", type);
		type = t;
	}
	float oldpos[num_motors];
	bool ok;
	if (t != type) {
		//debug("setting type to %d", type);
		space_types[t].free(this);
		if (!space_types[type].init(this)) {
			type = DEFAULT_TYPE;
			return;	// The rest of the package is not meant for DEFAULT_TYPE, so ignore it.
		}
		ok = false;
	}
	else {
		if (moving || !motors_busy)
			ok = false;
		else {
			for (uint8_t i = 0; i < num_axes; ++i)
				axis[i]->settings[current_fragment].target = axis[i]->current;
			ok = true;
			space_types[type].xyz2motors(this, oldpos, &ok);
		}
	}
	max_deviation = read_float(addr);
	space_types[type].load(this, t, addr);
	if (ok) {
		float newpos[num_motors];
		space_types[type].xyz2motors(this, newpos, &ok);
		uint8_t i;
		for (i = 0; i < num_motors; ++i) {
			if (oldpos[i] != newpos[i]) {
				move_to_current(this);
				break;
			}
		}
	}
	loaddebug("done loading space");
}

void Space::load_axis(uint8_t a, int32_t &addr)
{
	loaddebug("loading axis %d", a);
	float old_offset = axis[a]->offset;
	axis[a]->offset = read_float(addr);
	axis[a]->park = read_float(addr);
	axis[a]->park_order = read_8(addr);
	axis[a]->max_v = read_float(addr);
	axis[a]->min = read_float(addr);
	axis[a]->max = read_float(addr);
	if (axis[a]->offset != old_offset) {
		axis[a]->current += axis[a]->offset - old_offset;
		axis[a]->source += axis[a]->offset - old_offset;
		move_to_current(this);
	}
}

void Space::load_motor(uint8_t m, int32_t &addr)
{
	loaddebug("loading motor %d", m);
	uint16_t enable = motor[m]->enable_pin.write();
	float old_home_pos = motor[m]->home_pos;
	float old_steps_per_m = motor[m]->steps_per_m;
	motor[m]->step_pin.read(read_16(addr));
	motor[m]->dir_pin.read(read_16(addr));
	motor[m]->enable_pin.read(read_16(addr));
	motor[m]->limit_min_pin.read(read_16(addr));
	motor[m]->limit_max_pin.read(read_16(addr));
	motor[m]->sense_pin.read(read_16(addr));
	motor[m]->steps_per_m = read_float(addr);
	motor[m]->max_steps = read_8(addr);
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
	SET_INPUT(motor[m]->sense_pin);
	bool must_move = false;
	if (!isnan(motor[m]->home_pos)) {
		// Axes with a limit switch.
		if (old_steps_per_m != motor[m]->steps_per_m) {
			float diff = motor[m]->settings[current_fragment].current_pos / old_steps_per_m - motor[m]->home_pos;
			float f = motor[m]->home_pos + diff;
			int32_t cpdiff = f * motor[m]->steps_per_m + (f > 0 ? .49 : -.49) - motor[m]->settings[current_fragment].current_pos;
			motor[m]->settings[current_fragment].current_pos += cpdiff;
			motor[m]->settings[current_fragment].hwcurrent_pos += cpdiff;
			//debug("cp5 %d", motor[m]->current_pos);
			arch_addpos(id, m, cpdiff);
			must_move = true;
		}
		if (motors_busy && old_home_pos != motor[m]->home_pos) {
			float f = motor[m]->home_pos - old_home_pos;
			int32_t diff = f * motor[m]->steps_per_m + (f > 0 ? .49 : -.49);
			motor[m]->settings[current_fragment].current_pos += diff;
			motor[m]->settings[current_fragment].hwcurrent_pos += diff;
			//debug("cp6 %d", motor[m]->current_pos);
			arch_addpos(id, m, diff);
			must_move = true;
		}
	}
	else {
		// Axes without a limit switch: extruders.
		if (motors_busy && old_steps_per_m != motor[m]->steps_per_m) {
			int32_t cp = motor[m]->settings[current_fragment].current_pos;
			float pos = motor[m]->settings[current_fragment].current_pos / old_steps_per_m;
			motor[m]->settings[current_fragment].current_pos = pos * motor[m]->steps_per_m;
			motor[m]->settings[current_fragment].hwcurrent_pos += motor[m]->settings[current_fragment].current_pos - cp;
			arch_addpos(id, m, motor[m]->settings[current_fragment].current_pos - cp);
		}
	}
	if (must_move)
		move_to_current(this);
}

void Space::save_info(int32_t &addr)
{
	//debug("saving info %d %f %d %d", type, F(max_deviation), num_axes, num_motors);
	write_8(addr, type);
	write_float(addr, max_deviation);
	space_types[type].save(this, addr);
}

void Space::save_axis(uint8_t a, int32_t &addr) {
	write_float(addr, axis[a]->offset);
	write_float(addr, axis[a]->park);
	write_8(addr, axis[a]->park_order);
	write_float(addr, axis[a]->max_v);
	write_float(addr, axis[a]->min);
	write_float(addr, axis[a]->max);
}

void Space::save_motor(uint8_t m, int32_t &addr) {
	write_16(addr, motor[m]->step_pin.write());
	write_16(addr, motor[m]->dir_pin.write());
	write_16(addr, motor[m]->enable_pin.write());
	write_16(addr, motor[m]->limit_min_pin.write());
	write_16(addr, motor[m]->limit_max_pin.write());
	write_16(addr, motor[m]->sense_pin.write());
	write_float(addr, motor[m]->steps_per_m);
	write_8(addr, motor[m]->max_steps);
	write_float(addr, motor[m]->home_pos);
	write_float(addr, motor[m]->limit_v);
	write_float(addr, motor[m]->limit_a);
	write_8(addr, motor[m]->home_order);
}

int32_t Space::savesize_std() {
	return (2 * 6 + 4 * 6 + 1 * 2) * num_motors + sizeof(float) * (1 + 3 * num_axes + 3 * num_motors);
}

int32_t Space::savesize() {
	return 1 * 1 + space_types[type].savesize(this);
}

int32_t Space::savesize0() {
	return 2;	// Cartesian type, 0 axes.
}

void Space::init(uint8_t space_id) {
	type = DEFAULT_TYPE;
	id = space_id;
	max_deviation = 0;
	type_data = NULL;
	num_axes = 0;
	num_motors = 0;
	motor = NULL;
	axis = NULL;
}

void Space::free() {
	space_types[type].free(this);
	for (uint8_t a = 0; a < num_axes; ++a)
		delete axis[a];
	delete[] axis;
	for (uint8_t m = 0; m < num_motors; ++m) {
		motor[m]->step_pin.read(0);
		motor[m]->dir_pin.read(0);
		motor[m]->enable_pin.read(0);
		motor[m]->limit_min_pin.read(0);
		motor[m]->limit_max_pin.read(0);
		motor[m]->sense_pin.read(0);
		delete motor[m];
	}
	delete[] motor;
}

void Space::copy(Space &dst) {
	//debug("copy space");
	dst.type = type;
	dst.type_data = type_data;
	dst.num_axes = num_axes;
	dst.num_motors = num_motors;
	dst.axis = axis;
	dst.motor = motor;
}

void Space::cancel_update() {
	// setup_nums failed; restore system to a usable state.
	type = DEFAULT_TYPE;
	uint8_t n = min(num_axes, num_motors);
	if (!setup_nums(n, n)) {
		debug("Failed to free memory; erasing name and removing all motors and axes to make sure it works");
		delete[] name;
		namelen = 0;
		if (!setup_nums(0, 0))
			debug("You're in trouble; this shouldn't be possible");
	}
}

// Space things. {{{
static void check_distance(Motor *mtr, float distance, float dt, float &factor) { // {{{
	if (isnan(distance) || distance == 0) {
		mtr->settings[current_fragment].target_dist = 0;
		//mtr->last_v = 0;
		return;
	}
	//debug("cd %f %f", F(distance), F(dt));
	mtr->settings[current_fragment].target_dist = distance;
	mtr->settings[current_fragment].target_v = distance / dt;
	float v = fabs(mtr->settings[current_fragment].target_v);
	int8_t s = (mtr->settings[current_fragment].target_v < 0 ? -1 : 1);
	// When turning around, ignore limits (they shouldn't have been violated anyway).
	if (mtr->settings[current_fragment].last_v * s < 0) {
		movedebug("!");
		mtr->settings[current_fragment].last_v = 0;
	}
	// Limit v.
	if (v > mtr->limit_v) {
		movedebug("v %f %f", F(v), F(mtr->limit_v));
		distance = (s * mtr->limit_v) * dt;
		v = fabs(distance / dt);
	}
	//debug("cd2 %f %f", F(distance), F(dt));
	// Limit a+.
	float limit_dv = mtr->limit_a * dt;
	if (v - mtr->settings[current_fragment].last_v * s > limit_dv) {
		movedebug("a+ %f %f %f %d", F(mtr->settings[current_fragment].target_v), F(limit_dv), F(mtr->settings[current_fragment].last_v), s);
		distance = (limit_dv * s + mtr->settings[current_fragment].last_v) * dt;
		v = fabs(distance / dt);
	}
	//debug("cd3 %f %f", F(distance), F(dt));
	// Limit a-.
	// Distance to travel until end of segment or connection.
	float max_dist = (mtr->settings[current_fragment].endpos - mtr->settings[current_fragment].current_pos / mtr->steps_per_m) * s;
	// Find distance traveled when slowing down at maximum a.
	// x = 1/2 at²
	// t = sqrt(2x/a)
	// v = at
	// v² = 2a²x/a = 2ax
	// x = v²/2a
	float limit_dist = v * v / 2 / mtr->limit_a;
	//debug("max %f limit %f v %f a %f", max_dist, limit_dist, v, mtr->limit_a);
	if (max_dist > 0 && limit_dist > max_dist) {
		movedebug("a- %f %f %f %d %d %f", F(mtr->settings[current_fragment].endpos), F(mtr->limit_a), F(max_dist), F(mtr->settings[current_fragment].current_pos), s, dt);
		v = sqrt(max_dist * 2 * mtr->limit_a);
		distance = s * v * dt;
	}
	//debug("cd4 %f %f", F(distance), F(dt)); */
	float f = distance / mtr->settings[current_fragment].target_dist;
	//movedebug("checked %f %f", F(mtr->target_dist), F(distance));
	if (f < factor)
		factor = f;
} // }}}

static void move_axes(Space *s, uint32_t current_time, float &factor) { // {{{
	float motors_target[s->num_motors];
	bool ok = true;
	space_types[s->type].xyz2motors(s, motors_target, &ok);
	// Try again if it didn't work; it should have moved target to a better location.
	if (!ok)
		space_types[s->type].xyz2motors(s, motors_target, &ok);
	//movedebug("ok %d", ok);
	for (uint8_t m = 0; m < s->num_motors; ++m) {
		movedebug("move %d %f %d", m, F(motors_target[m]), F(s->motor[m]->settings[current_fragment].current_pos));
		check_distance(s->motor[m], motors_target[m] - s->motor[m]->settings[current_fragment].current_pos / s->motor[m]->steps_per_m, (current_time - settings[current_fragment].last_time) / 1e6, factor);
	}
} // }}}

static void do_steps(float &factor, uint32_t current_time) { // {{{
	//debug("steps");
	if (factor <= 0) {
		movedebug("end move");
		moving = false;
		return;
	}
	//movedebug("do steps %f %d", F(factor), max_steps);
	bool have_steps = false;
	for (uint8_t s = 0; !have_steps && s < num_spaces; ++s) {
		Space &sp = spaces[s];
		for (uint8_t m = 0; m < sp.num_motors; ++m) {
			Motor &mtr = *sp.motor[m];
			float num = (mtr.settings[current_fragment].current_pos / mtr.steps_per_m + mtr.settings[current_fragment].target_dist * factor) * mtr.steps_per_m;
			if (mtr.settings[current_fragment].current_pos != int(num + (num > 0 ? .49 : -.49))) {
				//debug("have steps %d %f %f", mtr.settings[current_fragment].current_pos, mtr.settings[current_fragment].target_dist, factor);
				have_steps = true;
				break;
			}
			//debug("no steps yet %d %d", s, m);
		}
	}
	uint32_t the_last_time = settings[current_fragment].last_current_time;
	settings[current_fragment].last_current_time = current_time;
	// If there are no steps to take, wait until there are.
	if (!have_steps) {
		movedebug("no steps");
		return;
	}
	// Adjust start time if factor < 1.
	if (factor > 0 && factor < 1) {
		settings[current_fragment].start_time += (current_time - the_last_time) * ((1 - factor) * .99);
		movedebug("correct: %f %d", F(factor), int(settings[current_fragment].start_time));
	}
	else
		movedebug("no correct: %f %d", F(factor), int(settings[current_fragment].start_time));
	settings[current_fragment].last_time = current_time;
	// Move the motors.
	//debug("start move");
	for (uint8_t s = 0; s < num_spaces; ++s) {
		Space &sp = spaces[s];
		for (uint8_t m = 0; m < sp.num_motors; ++m) {
			Motor &mtr = *sp.motor[m];
			if (mtr.settings[current_fragment].target_dist == 0) {
				//if (mtr.target_v == 0)
					//mtr.last_v = 0;
				continue;
			}
			float target = mtr.settings[current_fragment].current_pos / mtr.steps_per_m + mtr.settings[current_fragment].target_dist * factor;
			//debug("ccp3 %d %d %d %f %f %f %f %f", m, stopping, mtr.current_pos, F(target), F(mtr.last_v), mtr.steps_per_m, mtr.target_dist, factor);
			mtr.settings[current_fragment].current_pos = (target * mtr.steps_per_m + (target > 0 ? .49 : -.49));
			//debug("cp3 %d", mtr.current_pos);
			mtr.settings[current_fragment].last_v = mtr.settings[current_fragment].target_v * factor;
		}
	}
	for (uint8_t s = 0; s < num_spaces; ++s) {
		Space &sp = spaces[s];
		for (uint8_t a = 0; a < sp.num_axes; ++a)
			sp.axis[a]->current += (sp.axis[a]->settings[current_fragment].target - sp.axis[a]->current) * factor;
	}
	return;
} // }}}

static void handle_motors(unsigned long long current_time) { // {{{
	// Check for move.
	if (!moving)
		return;
	float factor = 1;
	float t = (current_time - settings[current_fragment].start_time) / 1e6;
	//buffered_debug("f%f %f %f %ld %ld", F(t), F(t0), F(tp), F(long(current_time)), F(long(settings[current_fragment].start_time)));
	if (t >= settings[current_fragment].t0 + settings[current_fragment].tp) {	// Finish this move and prepare next.
		movedebug("finishing %f %f %f %ld %ld", F(t), F(settings[current_fragment].t0), F(settings[current_fragment].tp), F(long(current_time)), F(long(settings[current_fragment].start_time)));
		//buffered_debug("a");
		for (uint8_t s = 0; s < num_spaces; ++s) {
			Space &sp = spaces[s];
			for (uint8_t a = 0; a < sp.num_axes; ++a) {
				//debug("before source %d %f %f", a, F(axis[a].source), F(axis[a].motor.dist));
				if (!isnan(sp.axis[a]->settings[current_fragment].dist)) {
					sp.axis[a]->source += sp.axis[a]->settings[current_fragment].dist;
					sp.axis[a]->settings[current_fragment].dist = NAN;
					// Set this here, so it isn't set if dist was NaN to begin with.
					// Note that target is not set for future iterations, but it isn't changed.
					sp.axis[a]->settings[current_fragment].target = sp.axis[a]->source;
				}
				//debug("after source %d %f %f %f %f", a, F(sp.axis[a]->source), F(sp.axis[a]->dist), F(sp.motor[a]->current_pos), F(factor));
			}
			move_axes(&sp, current_time, factor);
			//debug("f %f", F(factor));
		}
		//debug("f2 %f %ld %ld", F(factor), F(settings[current_fragment].last_time), F(current_time));
		do_steps(factor, current_time);
		//debug("f3 %f", F(factor));
		// Start time may have changed; recalculate t.
		t = (current_time - settings[current_fragment].start_time) / 1e6;
		if (t / (settings[current_fragment].t0 + settings[current_fragment].tp) >= done_factor) {
			//buffered_debug("b");
			moving = false;
			uint8_t had_cbs = cbs_after_current_move;
			cbs_after_current_move = 0;
			had_cbs += next_move();
			if (moving) {
				//buffered_debug("c");
				//debug("movecb 1");
				if (!aborting && had_cbs > 0)
					settings[current_fragment].cbs += had_cbs;
				return;
			}
			//buffered_debug("d");
			cbs_after_current_move += had_cbs;
			if (factor == 1) {
				//buffered_debug("e");
				moving = false;
				buffer_refill();
				for (uint8_t s = 0; s < num_spaces; ++s) {
					Space &sp = spaces[s];
					for (uint8_t m = 0; m < sp.num_motors; ++m)
						sp.motor[m]->settings[current_fragment].last_v = 0;
				}
				//debug("movecb 1");
				if (cbs_after_current_move > 0) {
					if (!aborting)
						settings[current_fragment].cbs += cbs_after_current_move;
					cbs_after_current_move = 0;
				}
			}
			else {
				moving = true;
				//if (factor > 0)
				//	debug("not done %f", F(factor));
			}
		}
		return;
	}
	if (t < settings[current_fragment].t0) {	// Main part.
		float t_fraction = t / settings[current_fragment].t0;
		float current_f = (settings[current_fragment].f1 * (2 - t_fraction) + settings[current_fragment].f2 * t_fraction) * t_fraction;
		movedebug("main t %f t0 %f tp %f tfrac %f f1 %f f2 %f cf %f", F(t), F(settings[current_fragment].t0), F(settings[current_fragment].tp), F(t_fraction), F(settings[current_fragment].f1), F(settings[current_fragment].f2), F(current_f));
		for (uint8_t s = 0; s < num_spaces; ++s) {
			Space &sp = spaces[s];
			for (uint8_t a = 0; a < sp.num_axes; ++a) {
				if (isnan(sp.axis[a]->settings[current_fragment].dist) || sp.axis[a]->settings[current_fragment].dist == 0) {
					sp.axis[a]->settings[current_fragment].target = NAN;
					continue;
				}
				sp.axis[a]->settings[current_fragment].target = sp.axis[a]->source + sp.axis[a]->settings[current_fragment].dist * current_f;
				//movedebug("do %d %d %f %f", s, a, F(sp.axis[a]->dist), F(target[a]));
			}
			move_axes(&sp, current_time, factor);
		}
	}
	else {	// Connector part.
		movedebug("connector %f %f %f", F(t), F(settings[current_fragment].t0), F(settings[current_fragment].tp));
		float tc = t - settings[current_fragment].t0;
		for (uint8_t s = 0; s < num_spaces; ++s) {
			Space &sp = spaces[s];
			for (uint8_t a = 0; a < sp.num_axes; ++a) {
				if ((isnan(sp.axis[a]->settings[current_fragment].dist) || sp.axis[a]->settings[current_fragment].dist == 0) && (isnan(sp.axis[a]->settings[current_fragment].next_dist) || sp.axis[a]->settings[current_fragment].next_dist == 0)) {
					sp.axis[a]->settings[current_fragment].target = NAN;
					continue;
				}
				float t_fraction = tc / settings[current_fragment].tp;
				float current_f2 = settings[current_fragment].fp * (2 - t_fraction) * t_fraction;
				float current_f3 = settings[current_fragment].fq * t_fraction * t_fraction;
				sp.axis[a]->settings[current_fragment].target = sp.axis[a]->source + sp.axis[a]->settings[current_fragment].main_dist + sp.axis[a]->settings[current_fragment].dist * current_f2 + sp.axis[a]->settings[current_fragment].next_dist * current_f3;
			}
			move_axes(&sp, current_time, factor);
		}
	}
	do_steps(factor, current_time);
} // }}}

void reset_dirs(int fragment) {
	//debug("resetting %d", fragment);
	settings[fragment].num_active_motors = 0;
	for (uint8_t s = 0; s < num_spaces; ++s) {
		Space &sp = spaces[s];
		for (uint8_t m = 0; m < sp.num_motors; ++m) {
			Motor &mtr = *sp.motor[m];
			if (moving && mtr.settings[fragment].current_pos != mtr.settings[fragment].hwcurrent_pos) {
				//debug("preactive %d %d %d %d %d", s, m, fragment, mtr.current_pos, mtr.hwcurrent_pos);
				settings[fragment].num_active_motors += 1;
				mtr.settings[fragment].dir = mtr.settings[fragment].current_pos < mtr.settings[fragment].hwcurrent_pos ? -1 : 1;
			}
			else {
				//debug("inactive %d %d %d", s, m, fragment);
				mtr.settings[fragment].dir = 0;
			}
		}
	}
}

void copy_fragment_settings(int src, int dst) {
	settings[dst].t0 = settings[src].t0;
	settings[dst].tp = settings[src].tp;
	settings[dst].f0 = settings[src].f0;
	settings[dst].f1 = settings[src].f1;
	settings[dst].f2 = settings[src].f2;
	settings[dst].fp = settings[src].fp;
	settings[dst].fq = settings[src].fq;
	settings[dst].fmain = settings[src].fmain;
	settings[dst].fragment_length = settings[src].fragment_length;
	settings[dst].num_active_motors = settings[src].num_active_motors;
	settings[dst].cbs = settings[src].cbs;
	settings[dst].hwtime = settings[src].hwtime;
	settings[dst].start_time = settings[src].start_time;
	settings[dst].last_time = settings[src].last_time;
	settings[dst].last_current_time = settings[src].last_current_time;
	for (int s = 0; s < num_spaces; ++s) {
		Space &sp = spaces[s];
		for (int m = 0; m < sp.num_motors; ++m) {
			//debug("set fragment %d %d %d %d %d %d", s, m, src, dst, sp.motor[m]->settings[src].hwcurrent_pos, sp.motor[m]->settings[src].current_pos);
			sp.motor[m]->settings[dst].dir = sp.motor[m]->settings[src].dir;
			sp.motor[m]->settings[dst].last_v = sp.motor[m]->settings[src].last_v;
			sp.motor[m]->settings[dst].target_v = sp.motor[m]->settings[src].target_v;
			sp.motor[m]->settings[dst].target_dist = sp.motor[m]->settings[src].target_dist;
			sp.motor[m]->settings[dst].current_pos = sp.motor[m]->settings[src].current_pos;
			sp.motor[m]->settings[dst].hwcurrent_pos = sp.motor[m]->settings[src].hwcurrent_pos;
			sp.motor[m]->settings[dst].endpos = sp.motor[m]->settings[src].endpos;
		}
		for (int a = 0; a < sp.num_axes; ++a) {
			sp.axis[a]->settings[dst].dist = sp.axis[a]->settings[src].dist;
			sp.axis[a]->settings[dst].next_dist = sp.axis[a]->settings[src].next_dist;
			sp.axis[a]->settings[dst].main_dist = sp.axis[a]->settings[src].main_dist;
			sp.axis[a]->settings[dst].target = sp.axis[a]->settings[src].target;
		}
	}
}

void set_current_fragment(int fragment) {
	if (!moving && current_fragment_pos > 0)
		send_fragment();
	copy_fragment_settings(current_fragment, fragment);
	settings[fragment].fragment_length = 0;
	settings[fragment].cbs = 0;
	current_fragment = fragment;
	debug("curf4 %d", current_fragment);
	current_fragment_pos = 0;
	reset_dirs(current_fragment);
}

void send_fragment() {
	if (current_fragment_pos == 0)
		return;
	debug("sending %d", current_fragment);
	settings[current_fragment].fragment_length = current_fragment_pos;
	free_fragments -= 1;
	arch_send_fragment(current_fragment);
	if (stopping)
		return;
	// Prepare new fragment.
	current_fragment_pos = 0;
	set_current_fragment((current_fragment + 1) % FRAGMENTS_PER_BUFFER);
	for (int s = 0; s < num_spaces; ++s) {
		Space &sp = spaces[s];
		for (int m = 0; m < sp.num_motors; ++m)
			memset(sp.motor[m]->settings[current_fragment].data, 0, BYTES_PER_FRAGMENT);
	}
}

void apply_tick() {
	// All directions are correct, or the fragment is sent and the new fragment is empty (and therefore all directions are correct).
	for (uint8_t s = 0; (s == 0 || current_fragment_pos > 0) && s < num_spaces; ++s) {
		Space &sp = spaces[s];
		for (uint8_t m = 0; m < sp.num_motors; ++m) {
			Motor &mtr = *sp.motor[m];
			int value = (mtr.settings[current_fragment].current_pos - mtr.settings[current_fragment].hwcurrent_pos) * mtr.settings[current_fragment].dir;
			if (value > 15)
				value = 15;
			mtr.settings[current_fragment].data[current_fragment_pos >> 1] |= value << (4 * (current_fragment_pos & 0x1));
			mtr.settings[current_fragment].hwcurrent_pos += mtr.settings[current_fragment].dir * value;
			//debug("move pos %d %d %d %d %d", m, settings[current_fragment].hwtime, sp.motor[m]->settings[current_fragment].current_pos, sp.motor[m]->settings[current_fragment].hwcurrent_pos, sp.motor[m]->settings[current_fragment].current_pos - sp.motor[m]->settings[current_fragment].hwcurrent_pos);
		}
	}
	current_fragment_pos += 1;
	settings[current_fragment].hwtime += hwtime_step;
	handle_motors(settings[current_fragment].hwtime);
}

void buffer_refill() {
	if (refilling || stopping)
		return;
	refilling = true;
	// Keep one free fragment, because we want to be able to rewind and use the buffer before the one currently active.
	while (moving && !stopping && free_fragments > 1) {
		debug("refill %d %d %d", current_fragment, current_fragment_pos, spaces[0].motor[0]->settings[current_fragment].current_pos);
		// fill fragment until full or dirchange.
		for (uint8_t s = 0; s < num_spaces && !stopping; ++s) {
			Space &sp = spaces[s];
			for (uint8_t m = 0; m < sp.num_motors && !stopping; ++m) {
				Motor &mtr = *sp.motor[m];
				if (mtr.settings[current_fragment].current_pos == mtr.settings[current_fragment].hwcurrent_pos)
					continue;
				if (mtr.settings[current_fragment].dir == 0) {
					//debug("active %d %d %d %d", s, m, F(mtr.current_pos), F(mtr.hwcurrent_pos));
					mtr.settings[current_fragment].dir = (mtr.settings[current_fragment].current_pos < mtr.settings[current_fragment].hwcurrent_pos ? -1 : 1);
					settings[current_fragment].num_active_motors += 1;
					continue;
				}
				if (current_fragment_pos > 0 && ((mtr.settings[current_fragment].dir < 0 && mtr.settings[current_fragment].current_pos > mtr.settings[current_fragment].hwcurrent_pos) || (mtr.settings[current_fragment].dir > 0 && mtr.settings[current_fragment].current_pos < mtr.settings[current_fragment].hwcurrent_pos))) {
					//debug("dir change %d %d", s, m);
					send_fragment();
					if (free_fragments <= max(0, FRAGMENTS_PER_BUFFER - MIN_BUFFER_FILL) && !stopping)
						arch_start_move();
					if (free_fragments <= 0 && !stopping) {
						refilling = false;
						return;
					}
					break;
				}
				//debug("no change %d %d %d %d", s, m, mtr.dir[current_fragment], current_fragment);
			}
		}
		if (stopping) {
			refilling = false;
			return;
		}
		apply_tick();
		debug("refill2 %d %d", current_fragment, spaces[0].motor[0]->settings[current_fragment].current_pos);
		if ((!moving && current_fragment_pos > 0) || current_fragment_pos >= BYTES_PER_FRAGMENT * 2) {
			//debug("fragment full %d %d %d", moving, current_fragment_pos, BYTES_PER_FRAGMENT * 2);
			if (free_fragments <= max(0, FRAGMENTS_PER_BUFFER - MIN_BUFFER_FILL) && !stopping)
				arch_start_move();
			send_fragment();
		}
		// Check for commands from host; in case of many short buffers, this loop may not end in a reasonable time.
		serial(0);
	}
	refilling = false;
	if (stopping)
		return;
	if (!moving)
		send_fragment();
	//debug("free %d/%d", free_fragments, FRAGMENTS_PER_BUFFER);
	if (free_fragments < FRAGMENTS_PER_BUFFER)
		arch_start_move();
}
// }}}
