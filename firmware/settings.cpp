#include "firmware.h"

void Constants::load (uint16_t &addr, bool eeprom)
{
	// This function must not be called.
	debug ("Constants::load must not be called!");
}

void Constants::save (uint16_t &addr, bool eeprom)
{
	write_8 (addr, MAXAXES, eeprom);
	write_8 (addr, MAXEXTRUDERS, eeprom);
	write_8 (addr, MAXTEMPS, eeprom);
}

void Variables::load (uint16_t &addr, bool eeprom)
{
	num_axes = read_8 (addr, eeprom);
	num_extruders = read_8 (addr, eeprom);
	num_temps = read_8 (addr, eeprom);
	led_pin = read_8 (addr, eeprom);
	room_T = read_float (addr, eeprom) + 273.15;
	motor_limit = read_32 (addr, eeprom);
	temp_limit = read_32 (addr, eeprom);
	// If settings are invalid values, the eeprom is probably not initialized; use defaults.
	if (num_axes > MAXAXES)
		num_axes = 3;
	if (num_extruders > MAXEXTRUDERS)
		num_extruders = 1;
	if (num_temps > MAXTEMPS)
		num_temps = 1;
	SET_OUTPUT (led_pin);
}

void Variables::save (uint16_t &addr, bool eeprom)
{
	write_8 (addr, num_axes, eeprom);
	write_8 (addr, num_extruders, eeprom);
	write_8 (addr, num_temps, eeprom);
	write_8 (addr, led_pin, eeprom);
	write_float (addr, room_T - 273.15, eeprom);
	write_32 (addr, motor_limit, eeprom);
	write_32 (addr, temp_limit, eeprom);
}
