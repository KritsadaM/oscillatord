#include <sys/select.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>

#include <error.h>

#include <oscillator-disciplining/oscillator-disciplining.h>

#include "log.h"
#include "oscillator.h"
#include "oscillator_factory.h"
#include "gnss.h"
#include "config.h"
#include "utils.h"

/*
 * The driver has a watchdog which resets the 1PPS device if no interrupt has
 * been received in the last two seconds, so a timeout of more than 4 seconds
 * means that even the watchdog couldn't "repair" the 1PPS device.
 */
#define LOOP_TIMEOUT 4

__attribute__((cleanup(gnss_cleanup))) struct gnss gnss = {0};

static void dummy_print_progname(void)
{
	fprintf(stderr, ERR);
}

static volatile int loop = true;

static void signal_handler(int signum)
{
	info("Caught signal %s.\n", strsignal(signum));
	if (!loop) {
		err("Signalled twice, brutal exit.\n");
		exit(EXIT_FAILURE);
	}
	loop = false;
}

static int apply_phase_offset(int fd, const char *device_name,
				  int32_t phase_error)
{
	int ret;

	ret = write(fd, &phase_error, sizeof(phase_error));
	if (ret == -1) {
		err("Can't write %s ", device_name);
		return -errno;
	}
	info("%s: applied a phase offset correction of %"PRIi32"ns\n",
			device_name, phase_error);

	return ret;
}


int main(int argc, char *argv[])
{
	fd_set rfds;
	struct od_input input;
	struct od_output output;
	int ret;
	struct timeval tv;
	ssize_t sret;
	int32_t phase_error;
	const char *device;
	struct config config;
	const char *path;
	const char *libod_conf_path;
	int pps_valid;
	bool opposite_phase_error;
	const char *value;
	int sign;
	unsigned int turns;
	char err_msg[OD_ERR_MSG_LEN];
	uint16_t temperature;

	__attribute__((cleanup(od_destroy))) struct od *od = NULL;
	__attribute__((cleanup(fd_cleanup))) int fd = -1;
	__attribute__((cleanup(oscillator_factory_destroy)))
			struct oscillator *oscillator = NULL;
	bool ignore_next_irq = false;

	/* remove the line startup in error() calls */
	error_print_progname = dummy_print_progname;

	if (argc != 2)
		error(EXIT_FAILURE, 0, "usage: %s config_file_path", argv[0]);
	path = argv[1];


	ret = config_init(&config, path);
	if (ret != 0)
		error(EXIT_FAILURE, -ret, "config_init(%s)", path);

	log_enable_debug(config_get_bool_default(&config, "debug", false));

	debug("Parsing configuration\n");
	value = config_get(&config, "turns");
	turns = (value != NULL) ? atoll(value) : 0;

	debug("Creating oscillator oscillator_factory_new\n");
	oscillator = oscillator_factory_new(&config);
	if (oscillator == NULL)
		error(EXIT_FAILURE, errno, "oscillator_factory_new");
	info("oscillator model %s\n", oscillator->class->name);

	struct oscillator_ctrl value_test;
	oscillator_get_ctrl(oscillator, &value_test);
	debug("Oscillator controls:\n");
	debug("fine value:%d\n", value_test.fine_ctrl);
	debug("coarse value:%d\n", value_test.coarse_ctrl);

	device = config_get(&config, "pps-device");
	if (device == NULL)
		error(EXIT_FAILURE, errno, "pps-device not defined in "
				"config %s", path);
	info("PPS device %s\n", device);

	fd = open(device, O_RDWR);
	if (fd == -1)
		error(EXIT_FAILURE, errno, "open(%s)", device);

	libod_conf_path = config_get_default(&config, "libod-config-path",
		path);
	
	debug("Creating library context\n");
	od = od_new_from_config(libod_conf_path, err_msg);

	if (od == NULL)
		error(EXIT_FAILURE, errno, "od_new %s", err_msg);
	oscillator_set_dac_min(oscillator, od_get_dac_min(od));
	oscillator_set_dac_max(oscillator, od_get_dac_max(od));


	opposite_phase_error = config_get_bool_default(&config,
			"opposite-phase-error", false);
	sign = opposite_phase_error ? -1 : 1;
	if (opposite_phase_error)
		info("taking the opposite of the phase error reported\n");


	debug("INIT GNSS\n");
	ret = gnss_init(&config, &gnss);
	if (ret < 0)
		error(EXIT_FAILURE, errno, "Failed to listen to the receiver");

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	info("Starting main loop\n");
	do {
		turns--;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);

		debug("Setting timeval and selecting fd\n");
		tv = (struct timeval) { .tv_sec = LOOP_TIMEOUT, .tv_usec = 0 };
		ret = select(fd + 1, &rfds, NULL, NULL, &tv);
		switch (ret) {
		case 0:
			error(EXIT_FAILURE, 0, "Timeout, shouldn't happen!");
			/* no fall through GCC, error(1, ...) doesn't return */
			__attribute__ ((fallthrough));

		case -1:
			if (errno == EINTR)
				continue;
			error(EXIT_FAILURE, errno, "select");
		}

		debug("reading phase error \n");
		sret = read(fd, &phase_error, sizeof(phase_error));
		if (sret == -1) {
			if (errno == EAGAIN || errno == EINTR)
				continue;

			error(EXIT_FAILURE, errno, "read");
		}


		if (ignore_next_irq) {
			info("ignoring 1 input due to phase jump\n");
			ignore_next_irq = false;
			continue;
		}

		debug("Getting oscillator temperature\n");
		ret = oscillator_get_temp(oscillator, &temperature);
		if (ret == -ENOSYS)
			temperature = 0;
		else if (ret < 0)
			error(EXIT_FAILURE, -ret, "oscillator_get_temp");

		debug("Getting GNSS data\n");
		ret = gnss_get_data(&gnss);
		pps_valid = false;

		switch (ret) {
		case GNSS_INVALID:
		case GNSS_WAITING:
			pps_valid = false;
			break;
		case GNSS_VALID:
			pps_valid = true;
			break;
		case GNSS_ERROR:
			error(EXIT_FAILURE, errno,
			      "Error polling receiver data");
			break;
		}

		struct oscillator_ctrl ctrl_values;
		oscillator_get_ctrl(oscillator, &ctrl_values);
		debug("Oscillator controls:\n");
		debug("fine value:%d\n", ctrl_values.fine_ctrl);
		debug("coarse value:%d\n", ctrl_values.coarse_ctrl);


		debug("Creating input structure\n");
		debug("Phase error is %d and sign is %d\n", phase_error, sign);
		input = (struct od_input) {
			.phase_error = (struct timespec) {
				.tv_sec = sign * phase_error / NS_IN_SECOND,
				.tv_nsec = sign * phase_error % NS_IN_SECOND,
			},
			.valid = pps_valid,
			.lock = ctrl_values.lock,
			.temperature = temperature,
			.qErr = gnss.data.qErr,
			.fine_setpoint = ctrl_values.fine_ctrl,
			.coarse_setpoint = ctrl_values.coarse_ctrl,
		};
		info("input: phase_error = (%lds, %09ldns),"
			"valid = %s, lock = %s, qErr = %d, fine = %d, coarse = %d\n",
			input.phase_error.tv_sec,
			input.phase_error.tv_nsec,
			input.valid ? "true" : "false",
			input.lock ? "true" : "false",
			input.qErr,
			input.fine_setpoint,
			input.coarse_setpoint);

		debug("Calling od process !\n");
		ret = od_process(od, &input, &output);
		if (ret < 0)
			error(EXIT_FAILURE, -ret, "od_process");

		debug("output: setpoint = %"PRIu32", "
			"output_action = %d, "
			"value_phase_ctrl = %"PRIi32"ns\n",
			output.setpoint,
			output.action,
			output.value_phase_ctrl);

		if (output.action == ADJUST_COARSE) {
			info("Coarse adjustment to value %d requested !\n", output.setpoint);
		} else if (output.action == ADJUST_FINE) {
			info("Fine adjustement to value %d requested !\n", output.setpoint);
		}

		if (output.action == PHASE_JUMP) {
			info("Phase jump requested \n");
				ret = apply_phase_offset(fd, device, -output.value_phase_ctrl);
				if (ret < 0)
					error(EXIT_FAILURE, -ret, "apply_phase_offset");
				ignore_next_irq = true;
		} else if (output.action == CALIBRATE) {
			info("Calibration requested\n");
			debug("Calling oscillator_get_calibration_parameters\n");
			struct calibration_parameters * calib_params = od_get_calibration_parameters(od);
			if (calib_params == NULL) {
				error(EXIT_FAILURE, -ENOMEM, "od_get_calibration_parameters");
			}
			debug("Calling oscillator_calibrate\n");
			struct calibration_results *results = oscillator_calibrate(oscillator, calib_params, fd, sign);
			if (results == NULL) {
				error(EXIT_FAILURE, -ENOMEM, "oscillator_calibrate");
			}
			od_calibrate(od, calib_params, results);
		} else {
			debug("calling apply_output\n");
			ret = oscillator_apply_output(oscillator, &output);
			if (ret < 0)
				error(EXIT_FAILURE, -ret, "oscillator_apply_output");
		}
		sleep(5);
	} while (loop && turns != 1);

	od_destroy(&od);

	return EXIT_SUCCESS;
}
