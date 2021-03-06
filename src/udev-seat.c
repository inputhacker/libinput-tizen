/*
 * Copyright © 2013 Intel Corporation
 * Copyright © 2013-2015 Red Hat, Inc.
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "evdev.h"
#include "udev-seat.h"

static const char default_seat[] = "seat0";
static const char default_seat_name[] = "default";

char *_udev_monitor_event_source = NULL;
int _udev_monitor_buffer_size = 0;

static char *
_udev_get_udev_monitor_event_source(void)
{
	return _udev_monitor_event_source;
}

static int
_udev_get_udev_monitor_buffer_size(void)
{
	return _udev_monitor_buffer_size;
}

static struct udev_seat *
udev_seat_create(struct udev_input *input,
		 const char *device_seat,
		 const char *seat_name);
static struct udev_seat *
udev_seat_get_named(struct udev_input *input, const char *seat_name);

static bool
libinput_path_has_device(struct libinput *libinput, const char *devnode)
{
	struct device_node *dev;
	struct list *dev_list;

	if (!devnode) return false;
	dev_list = libinput_path_get_devices();
	if (dev_list->prev == NULL && dev_list->next == NULL) return false;

	list_for_each(dev, dev_list, link) {
		const char *name = dev->devname;
		if (!name) break;
		if (strcmp(name, devnode) == 0)
			return true;
	}

	return false;
}

static int
device_added(struct udev_device *udev_device,
	     struct udev_input *input,
	     const char *seat_name)
{
	struct evdev_device *device;
	const char *devnode, *sysname;
	const char *device_seat, *output_name;
	struct udev_seat *seat;

	device_seat = udev_device_get_property_value(udev_device, "ID_SEAT");
	if (!device_seat)
		device_seat = default_seat;

	if (!streq(device_seat, input->seat_id))
		return 0;

	if (ignore_litest_test_suite_device(udev_device))
		return 0;

	devnode = udev_device_get_devnode(udev_device);
	sysname = udev_device_get_sysname(udev_device);

	/* Search for matching logical seat */
	if (!seat_name)
		seat_name = udev_device_get_property_value(udev_device, "WL_SEAT");
	if (!seat_name)
		seat_name = default_seat_name;

	seat = udev_seat_get_named(input, seat_name);

	if (seat)
		libinput_seat_ref(&seat->base);
	else {
		seat = udev_seat_create(input, device_seat, seat_name);
		if (!seat)
			return -1;
	}

	if (libinput_path_has_device(&input->base, devnode))
	{
		log_info(&input->base, "libinput_path already created input device '%s.\n", devnode);
		return 0;
	}
	device = evdev_device_create(&seat->base, udev_device);
	libinput_seat_unref(&seat->base);

	if (device == EVDEV_UNHANDLED_DEVICE) {
		log_info(&input->base,
			 "%-7s - not using input device '%s'\n",
			 sysname,
			 devnode);
		return 0;
	} else if (device == NULL) {
		log_info(&input->base,
			 "%-7s - failed to create input device '%s'\n",
			 sysname,
			 devnode);
		return 0;
	}

	evdev_read_calibration_prop(device);

	output_name = udev_device_get_property_value(udev_device, "WL_OUTPUT");
	if (output_name)
		device->output_name = strdup(output_name);

	return 0;
}

static void
device_removed(struct udev_device *udev_device, struct udev_input *input)
{
	struct evdev_device *device, *next;
	struct udev_seat *seat;
	const char *syspath;

	syspath = udev_device_get_syspath(udev_device);
	list_for_each(seat, &input->base.seat_list, base.link) {
		list_for_each_safe(device, next,
				   &seat->base.devices_list, base.link) {
			if (streq(syspath,
				  udev_device_get_syspath(device->udev_device))) {
				evdev_device_remove(device);
				break;
			}
		}
	}
}

static int
udev_input_add_devices(struct udev_input *input, struct udev *udev)
{
	struct udev_enumerate *e;
	struct udev_list_entry *entry;
	struct udev_device *device;
	const char *path, *sysname;

	e = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(e, "input");
	udev_enumerate_scan_devices(e);
	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
		path = udev_list_entry_get_name(entry);
		device = udev_device_new_from_syspath(udev, path);
		if (!device)
			continue;

		sysname = udev_device_get_sysname(device);
		if (strncmp("event", sysname, 5) != 0) {
			udev_device_unref(device);
			continue;
		}

		if (device_added(device, input, NULL) < 0) {
			udev_device_unref(device);
			udev_enumerate_unref(e);
			return -1;
		}

		udev_device_unref(device);
	}
	udev_enumerate_unref(e);

	return 0;
}

static void
evdev_udev_handler(void *data)
{
	struct udev_input *input = data;
	struct udev_device *udev_device;
	const char *action;

	udev_device = udev_monitor_receive_device(input->udev_monitor);
	if (!udev_device)
		return;

	action = udev_device_get_action(udev_device);
	if (!action)
		goto out;

	if (strncmp("event", udev_device_get_sysname(udev_device), 5) != 0)
		goto out;

	if (streq(action, "add"))
		device_added(udev_device, input, NULL);
	else if (streq(action, "remove"))
		device_removed(udev_device, input);

out:
	udev_device_unref(udev_device);
}

static void
udev_input_remove_devices(struct udev_input *input)
{
	struct evdev_device *device, *next;
	struct udev_seat *seat, *tmp;

	list_for_each_safe(seat, tmp, &input->base.seat_list, base.link) {
		libinput_seat_ref(&seat->base);
		list_for_each_safe(device, next,
				   &seat->base.devices_list, base.link) {
			evdev_device_remove(device);
		}
		libinput_seat_unref(&seat->base);
	}
}

static void
udev_input_disable(struct libinput *libinput)
{
	struct udev_input *input = (struct udev_input*)libinput;

	if (!input->udev_monitor)
		return;

	udev_monitor_unref(input->udev_monitor);
	input->udev_monitor = NULL;
	libinput_remove_source(&input->base, input->udev_monitor_source);
	input->udev_monitor_source = NULL;

	udev_input_remove_devices(input);
}

static int
udev_input_enable(struct libinput *libinput)
{
	struct udev_input *input = (struct udev_input*)libinput;
	struct udev *udev = input->udev;
	int fd;
	unsigned int buf_size = 0;

	if (input->udev_monitor)
		return 0;

	char *env = _udev_get_udev_monitor_event_source();

	if (env)
	{
		input->udev_monitor = udev_monitor_new_from_netlink(udev, env);
		log_info(libinput, "udev: event source is %s.\n", env);
	}
	else
	{
		input->udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
		log_info(libinput, "udev: event source is udev (default event source).\n");
	}

	if (!input->udev_monitor) {
		log_info(libinput,
			 "udev: failed to create the udev monitor\n");
		return -1;
	}

    buf_size = _udev_get_udev_monitor_buffer_size();

	if (buf_size)
	{
		log_info(libinput,"udev: set receive buffer size = %d\n", buf_size);
		udev_monitor_set_receive_buffer_size(input->udev_monitor, buf_size);
	}

	udev_monitor_filter_add_match_subsystem_devtype(input->udev_monitor,
			"input", NULL);

	if (udev_monitor_enable_receiving(input->udev_monitor)) {
		log_info(libinput, "udev: failed to bind the udev monitor\n");
		udev_monitor_unref(input->udev_monitor);
		input->udev_monitor = NULL;
		return -1;
	}

	fd = udev_monitor_get_fd(input->udev_monitor);
	input->udev_monitor_source = libinput_add_fd(&input->base,
						     fd,
						     evdev_udev_handler,
						     input);
	if (!input->udev_monitor_source) {
		udev_monitor_unref(input->udev_monitor);
		input->udev_monitor = NULL;
		return -1;
	}

	if (udev_input_add_devices(input, udev) < 0) {
		udev_input_disable(libinput);
		return -1;
	}

	return 0;
}

static void
udev_input_destroy(struct libinput *input)
{
	struct udev_input *udev_input = (struct udev_input*)input;

	if (input == NULL)
		return;

	udev_unref(udev_input->udev);
	free(udev_input->seat_id);
}

static void
udev_seat_destroy(struct libinput_seat *seat)
{
	struct udev_seat *useat = (struct udev_seat*)seat;
	free(useat);
}

static struct udev_seat *
udev_seat_create(struct udev_input *input,
		 const char *device_seat,
		 const char *seat_name)
{
	struct udev_seat *seat;

	seat = zalloc(sizeof *seat);
	if (!seat)
		return NULL;

	libinput_seat_init(&seat->base, &input->base,
			   device_seat, seat_name,
			   udev_seat_destroy);

	return seat;
}

static struct udev_seat *
udev_seat_get_named(struct udev_input *input, const char *seat_name)
{
	struct udev_seat *seat;

	list_for_each(seat, &input->base.seat_list, base.link) {
		if (streq(seat->base.logical_name, seat_name))
			return seat;
	}

	return NULL;
}

static int
udev_device_change_seat(struct libinput_device *device,
			const char *seat_name)
{
	struct libinput *libinput = device->seat->libinput;
	struct udev_input *input = (struct udev_input *)libinput;
	struct evdev_device *evdev = evdev_device(device);
	struct udev_device *udev_device = evdev->udev_device;
	int rc;

	udev_device_ref(udev_device);
	device_removed(udev_device, input);
	rc = device_added(udev_device, input, seat_name);
	udev_device_unref(udev_device);

	return rc;
}

static const struct libinput_interface_backend interface_backend = {
	.resume = udev_input_enable,
	.suspend = udev_input_disable,
	.destroy = udev_input_destroy,
	.device_change_seat = udev_device_change_seat,
};

LIBINPUT_EXPORT void
libinput_udev_set_udev_monitor_event_source(const char *source)
{
	if (source)
	{
		if (_udev_monitor_event_source)
		{
			free(_udev_monitor_event_source);
			_udev_monitor_event_source = NULL;
		}

		_udev_monitor_event_source = strdup(source);
	}
}

LIBINPUT_EXPORT int
libinput_udev_set_udev_monitor_buffer_size(int size)
{
	if (0 >= size)
		return -1;

	_udev_monitor_buffer_size = size;
	return 0;
}

LIBINPUT_EXPORT struct libinput *
libinput_udev_create_context(const struct libinput_interface *interface,
			     void *user_data,
			     struct udev *udev)
{
	struct udev_input *input;

	if (!interface || !udev)
		return NULL;

	input = zalloc(sizeof *input);
	if (!input)
		return NULL;

	if (libinput_init(&input->base, interface,
			  &interface_backend, user_data) != 0) {
		libinput_unref(&input->base);
		free(input);
		return NULL;
	}

	input->udev = udev_ref(udev);
	_udev_monitor_event_source = strdup("udev");

	return &input->base;
}

LIBINPUT_EXPORT int
libinput_udev_assign_seat(struct libinput *libinput,
			  const char *seat_id)
{
	struct udev_input *input = (struct udev_input*)libinput;

	if (!seat_id)
		return -1;

	if (libinput->interface_backend != &interface_backend) {
		log_bug_client(libinput, "Mismatching backends.\n");
		return -1;
	}

	if (input->seat_id != NULL)
		return -1;

	input->seat_id = strdup(seat_id);

	if (udev_input_enable(&input->base) < 0)
		return -1;

	return 0;
}
