//
// TNG Base Raspberry Pi CMX Image
// Copyright (C) 2020 Matthias Bolte <matthias@tinkerforge.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//

// http://landley.net/writing/rootfs-howto.html
// https://wiki.gentoo.org/wiki/Custom_Initramfs
// http://jootamam.net/howto-initramfs-image.htm
// https://www.kernel.org/doc/Documentation/filesystems/overlayfs.txt
// http://git.busybox.net/busybox/tree/util-linux/switch_root.c
// http://www.stlinux.com/howto/initramfs

#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/random.h>
#include <crypt.h>
#include <sys/utsname.h>
#include <libkmod.h>
#include <zlib.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <dirent.h>
#include <sys/socket.h>
#include <linux/sockios.h>
#include <linux/netlink.h>
#include <linux/ethtool.h>
#include <net/if.h>
#include <linux/rtc.h>
#include <time.h>
#include <sys/time.h>
#include <libmount.h>

#define RTC_PATH "/dev/rtc0"
#define EEPROM_PATH "/dev/i2c-1"
#define EEPROM_ADDRESS 0x50
#define EEPROM_MAGIC_NUMBER 0x21474E54
#define ACCOUNT_NAME "tng"
#define DEFAULT_PASSWORD "default-tng-password"
#define SHADOW_PATH "/mnt/etc/shadow"
#define SHADOW_BACKUP_PATH SHADOW_PATH"-"
#define SHADOW_TMP_PATH SHADOW_PATH"+"
#define SHADOW_BUFFER_LENGTH (512 * 1024)
#define SHADOW_ENCRYPTED_LENGTH 512
#define ETHERNET_DEVICE_PATH "/sys/devices/platform/soc/3f980000.usb/usb1/1-1/1-1.7/1-1.7:1.0/"
#define ETHERNET_CONFIG_LENGTH 256

typedef struct {
	uint32_t magic_number; // magic number 0x21474E54 (TNG!)
	uint32_t checksum; // zlib CRC32 checksum over all following bytes
	uint16_t data_length; // length of data blocks in byte
	uint8_t data_version; // indicating the available data blocks
} __attribute__((packed)) EEPROM_Header;

typedef struct {
	uint32_t production_date; // BCD formatted production date (0x20200827 -> 2020-08-27), exposed at /etc/tng-base-production-date
	char uid[7]; // null-terminated unique identifier, exposed at /etc/tng-base-uid
	char hostname[65]; // null-terminated /etc/hostname entry, also exposed at /etc/tng-base-hostname
	char encrypted_password[107]; // null-terminated /etc/shadow password entry
	uint8_t ethernet_config[ETHERNET_CONFIG_LENGTH]; // config for Ethernet chip
} __attribute__((packed)) EEPROM_DataV1;

typedef struct {
	EEPROM_Header header;
	EEPROM_DataV1 data_v1;
} __attribute__((packed)) EEPROM;

static int kmsg_fd = -1;
static EEPROM eeprom;
static bool eeprom_valid = false;

static void print(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void panic(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void vprint(const char *prefix, const char *format, va_list ap)
{
	char message[512];
	char buffer[512];
	int ignored;

	vsnprintf(message, sizeof(message), format, ap);

	if (kmsg_fd < 0) {
		printf("initramfs: %s%s\n", prefix, message);
	} else {
		snprintf(buffer, sizeof(buffer), "initramfs: %s%s\n", prefix, message);

		ignored = write(kmsg_fd, buffer, strlen(buffer)); // FIXME: error handling

		(void)ignored;
	}
}

static void print(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vprint("", format, ap);
	va_end(ap);
}

static void error(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vprint("error: ", format, ap);
	va_end(ap);
}

static void panic(const char *format, ...)
{
	va_list ap;
	FILE *fp;
	int i;

	if (format != NULL) {
		va_start(ap, format);
		vprint("panic: ", format, ap);
		va_end(ap);
	}

	// ensure /proc is mounted
	if (mkdir("/proc", 0775) < 0) {
		if (errno != EEXIST) {
			error("could not create /proc: %s (%d)", strerror(errno), errno);
		} else {
			errno = 0; // don't leak errno
		}
	}

	if (mount("proc", "/proc", "proc", 0, "") < 0) {
		if (errno != EBUSY) {
			error("could not mount proc at /proc: %s (%d)", strerror(errno), errno);
		} else {
			errno = 0; // don't leak errno
		}
	}

	// wait 60 seconds
	print("triggering reboot in 60 sec");
	sleep(50);

	print("triggering reboot in 10 sec");
	sleep(5);

	for (i = 5; i > 0; --i) {
		print("triggering reboot in %d sec", i);
		sleep(1);
	}

	// trigger reboot
	fp = fopen("/proc/sysrq-trigger", "wb");

	if (fp == NULL) {
		error("could not open /proc/sysrq-trigger for writing: %s (%d)", strerror(errno), errno);
	} else {
		if (fwrite("b\n", 1, 2, fp) != 2) {
			error("could not write reboot request to /proc/sysrq-trigger");
		} else {
			print("reboot triggered");
		}

		fclose(fp);
	}

	// wait for reboot to happen
	while (true) {
		sleep(1000);
	}
}

static void robust_mount(const char *source, const char *target, const char *type, unsigned long flags)
{
	struct libmnt_context *ctx;
	int rc;
	char buffer[512] = "<unknown>";
	int ex;
	size_t retries = 0;

	print("mounting %s (%s) at %s", source, type, target);

retry:
	ctx = mnt_new_context();

	if (ctx == NULL) {
		panic("could not create libmount context");
	}

	rc = mnt_context_disable_helpers(ctx, true);

	if (rc < 0) {
		panic("could not disable libmount helpers: %s (%d)", strerror(-rc), -rc);
	}

	rc = mnt_context_set_fstype(ctx, type);

	if (rc < 0) {
		panic("could not set libmount fstype to %s: %s (%d)", type, strerror(-rc), -rc);
	}

	rc = mnt_context_set_source(ctx, source);

	if (rc < 0) {
		panic("could not set libmount source to %s: %s (%d)", source, strerror(-rc), -rc);
	}

	rc = mnt_context_set_target(ctx, target);

	if (rc < 0) {
		panic("could not set libmount target to %s: %s (%d)", target, strerror(-rc), -rc);
	}

	rc = mnt_context_set_mflags(ctx, flags);

	if (rc < 0) {
		panic("could not set libmount flags to 0x%08lx: %s (%d)", flags, strerror(-rc), -rc);
	}

	rc = mnt_context_mount(ctx);

	if (rc != 0) {
		if (rc == -MNT_ERR_NOSOURCE) {
			error("could not mount %s (%s) at %s, device is missing, trying again in 500 msec", source, type, target);

			// fully recreate context for each try. the mnt_reset_context function
			// could be used here, but it doesn't invalidate/refresh the blkid cache
			// and there seems to be no other function to enforce that. but without
			// forcing a blkid cache update it takes around 200 seconds for a libmnt
			// context to realize that a new device has arrived.
			mnt_free_context(ctx);

			usleep(500 * 1000);

			++retries;

			goto retry;
		}

		ex = mnt_context_get_excode(ctx, rc, buffer, sizeof(buffer));

		panic("could not mount %s (%s) at %s: %s (%d -> %d)", source, type, target, buffer, rc, ex);
	}

	mnt_free_context(ctx);

	if (retries > 0) {
		print("successfully mounted %s (%s) at %s after %zu %s", source, type, target, retries, retries == 1 ? "retry" : "retries");
	}
}

static int create_file(const char *path, uid_t uid, gid_t gid, mode_t mode)
{
	int fd;

	print("creating %s", path);

	fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, mode);

	if (fd < 0) {
		panic("could not create %s for writing: %s (%d)", path, strerror(errno), errno);
	}

	if (fchown(fd, uid, gid) < 0) {
		panic("could not change owner of %s to %u:%u: %s (%d)", path, uid, gid, strerror(errno), errno);
	}

	if (fchmod(fd, mode) < 0) {
		panic("could not change mode of %s to 0o%03o: %s (%d)", path, mode, strerror(errno), errno);
	}

	return fd;
}

static void robust_write(const char *path, int fd, const void *buffer, size_t buffer_length)
{
	ssize_t length = write(fd, buffer, buffer_length);

	if (length < 0) {
		panic("could not write to %s: %s (%d)", path, strerror(errno), errno);
	}

	if ((size_t)length < buffer_length) {
		panic("short write to %s: %s (%d)", path, strerror(errno), errno);
	}
}

static void modprobe(const char *name)
{
	int rc;
	struct utsname utsname;
	char base[128];
	struct kmod_ctx *ctx;
	struct kmod_list *list = NULL;
	struct kmod_list *iter;
	struct kmod_module *module;

	print("loading kernel module %s", name);

	rc = uname(&utsname);

	if (rc < 0) {
		panic("could not get kernel release: %s (%d)", strerror(errno), errno);
	}

	snprintf(base, sizeof(base), "/mnt/lib/modules/%s", utsname.release);

	ctx = kmod_new(base, NULL);

	if (ctx == NULL) {
		panic("could not create kmod context");
	}

	rc = kmod_module_new_from_lookup(ctx, name, &list);

	if (rc < 0) {
		panic("could not lookup kernel module %s: %s (%d)", name, strerror(-rc), -rc);
	}

	if (list == NULL) {
		panic("kernel module %s is missing", name);
	}

	kmod_list_foreach(iter, list) {
		module = kmod_module_get_module(iter);
		rc = kmod_module_probe_insert_module(module, 0, NULL, NULL, NULL, NULL);

		if (rc < 0) {
			panic("could not load kernel module %s: %s (%d)", name, strerror(-rc), -rc);
		}

		kmod_module_unref(module);
	}

	kmod_module_unref_list(list);
}

static int i2c_write16(int fd, uint8_t byte0, uint8_t byte1)
{
	struct i2c_smbus_ioctl_data args;
	union i2c_smbus_data data;

	args.read_write = I2C_SMBUS_WRITE;
	args.command = byte0;
	args.size = I2C_SMBUS_BYTE_DATA;
	args.data = &data;

	data.byte = byte1;

	ioctl(fd, BLKFLSBUF);

	return ioctl(fd, I2C_SMBUS, &args);
}

static int i2c_read8(int fd, uint8_t *byte)
{
	struct i2c_smbus_ioctl_data args;
	union i2c_smbus_data data;
	int rc;

	args.read_write = I2C_SMBUS_READ;
	args.command = 0;
	args.size = I2C_SMBUS_BYTE;
	args.data = &data;

	ioctl(fd, BLKFLSBUF);

	rc = ioctl(fd, I2C_SMBUS, &args);

	if (rc < 0) {
		return rc;
	}

	*byte = data.byte & 0xFF;

	return 0;
}

static void rtc_hctosys(void)
{
	int fd;
	struct tm hc_start;
	struct tm hc_now;
	time_t timeout_start;
	time_t timeout_now;
	struct timeval sys_now;
	struct tm sys_now_local;
	int minuteswest;
	struct timezone tz;

	// read RTC time
	fd = open(RTC_PATH, O_RDONLY);

	if (fd < 0) {
		error("could not open %s for reading: %s (%d)", RTC_PATH, strerror(errno), errno);

		return;
	}

	if (ioctl(fd, RTC_RD_TIME, &hc_start) < 0) {
		error("could not read RTC time: %s (%d)", strerror(errno), errno);
		close(fd);

		return;
	}

	timeout_start = time(NULL);

	while (true) {
		if (ioctl(fd, RTC_RD_TIME, &hc_now) < 0) {
			error("could not read RTC time: %s (%d)", strerror(errno), errno);
			close(fd);

			return;
		}

		if (hc_start.tm_sec != hc_now.tm_sec) {
			break;
		}

		timeout_now = time(NULL);

		if (timeout_now - timeout_start > 3) {
			error("RTC time seems to be stuck, cannot set system time");
			close(fd);

			return;
		}
	}

	hc_now.tm_isdst = -1; // daylight saving time is undefined

	close(fd);

	// set system time
	sys_now.tv_sec = timegm(&hc_now);
	sys_now.tv_usec = 0;

	if (sys_now.tv_sec < 0) {
		error("could not convert RTC time %d-%02d-%02d %02d:%02d:%02d UTC to system time: %s (%d)",
		      hc_now.tm_year + 1900, hc_now.tm_mon + 1, hc_now.tm_mday,
		      hc_now.tm_hour, hc_now.tm_min, hc_now.tm_sec, strerror(errno), errno);

		return;
	}

	localtime_r(&sys_now.tv_sec, &sys_now_local);

	minuteswest = timezone / 60;

	if (sys_now_local.tm_isdst) {
		minuteswest -= 60;
	}

	tz.tz_minuteswest = minuteswest;
	tz.tz_dsttime = 0;

	if (settimeofday(&sys_now, &tz) < 0) {
		error("could not use RTC time %d-%02d-%02d %02d:%02d:%02d UTC as system time %d-%02d-%02d %02d:%02d:%02d %+03d:%02d: %s (%d)",
		      hc_now.tm_year + 1900, hc_now.tm_mon + 1, hc_now.tm_mday,
		      hc_now.tm_hour, hc_now.tm_min, hc_now.tm_sec,
		      sys_now_local.tm_year + 1900, sys_now_local.tm_mon + 1, sys_now_local.tm_mday,
		      sys_now_local.tm_hour, sys_now_local.tm_min, sys_now_local.tm_sec,
		      -minuteswest / 60, abs(minuteswest) % 60,
		      strerror(errno), errno);

		return;
	}

	print("using RTC time %d-%02d-%02d %02d:%02d:%02d UTC as system time %d-%02d-%02d %02d:%02d:%02d %+03d:%02d",
	      hc_now.tm_year + 1900, hc_now.tm_mon + 1, hc_now.tm_mday,
	      hc_now.tm_hour, hc_now.tm_min, hc_now.tm_sec,
	      sys_now_local.tm_year + 1900, sys_now_local.tm_mon + 1, sys_now_local.tm_mday,
	      sys_now_local.tm_hour, sys_now_local.tm_min, sys_now_local.tm_sec,
	      -minuteswest / 60, abs(minuteswest) % 60);
}

static void read_eeprom(void)
{
	int fd;
	size_t address;
	union {
		EEPROM eeprom;
		uint8_t bytes[sizeof(EEPROM)];
	} u;
	uint8_t byte;
	uint32_t checksum;

	eeprom_valid = false;

	// open I2C bus
	print("opening %s", EEPROM_PATH);

	fd = open(EEPROM_PATH, O_RDWR);

	if (fd < 0) {
		error("could not open %s: %s (%d)", EEPROM_PATH, strerror(errno), errno);

		return;
	}

	// set slave address
	if (ioctl(fd, I2C_SLAVE, EEPROM_ADDRESS) < 0) {
		error("could not set EEPROM slave address to 0x%02X: %s (%d)", EEPROM_ADDRESS, strerror(errno), errno);
		print("closing %s", EEPROM_PATH);
		close(fd);

		return;
	}

	// set read address to 0
	if (i2c_write16(fd, 0, 0) < 0) {
		error("could not set EEPROM read address to zero: %s (%d)", strerror(errno), errno);
		print("closing %s", EEPROM_PATH);
		close(fd);

		return;
	}

	// read header
	print("reading EEPROM header");

	for (address = 0; address < sizeof(u.eeprom.header); ++address) {
		if (i2c_read8(fd, &u.bytes[address]) < 0) {
			error("could not read EEPROM header at address %zu: %s (%d)", address, strerror(errno), errno);
			print("closing %s", EEPROM_PATH);
			close(fd);

			return;
		}
	}

	if (u.eeprom.header.magic_number != EEPROM_MAGIC_NUMBER) {
		error("EEPROM header has wrong magic number: %08X (actual) != %08X (expected)", u.eeprom.header.magic_number, EEPROM_MAGIC_NUMBER);
		print("closing %s", EEPROM_PATH);
		close(fd);

		return;
	}

	// read data
	print("reading EEPROM data");

	checksum = crc32(0, Z_NULL, 0);
	checksum = crc32(checksum, (uint8_t *)&u.eeprom.header.data_length, sizeof(u.eeprom.header.data_length));
	checksum = crc32(checksum, (uint8_t *)&u.eeprom.header.data_version, sizeof(u.eeprom.header.data_version));

	for (address = sizeof(u.eeprom.header); address < sizeof(u.eeprom.header) + u.eeprom.header.data_length; ++address) {
		if (i2c_read8(fd, &byte) < 0) {
			error("could not read EEPROM data at address %zu: %s (%d)", address, strerror(errno), errno);
			print("closing %s", EEPROM_PATH);
			close(fd);

			return;
		}

		if (address < sizeof(u.eeprom)) {
			u.bytes[address] = byte;
		}

		checksum = crc32(checksum, &byte, sizeof(byte));
	}

	print("closing %s", EEPROM_PATH);

	close(fd);

	// check header and data
	if (u.eeprom.header.checksum != checksum) {
		error("EEPROM header/data has wrong checksum: %08X (actual) != %08X (expected)", checksum, u.eeprom.header.checksum);

		return;
	}

	if (u.eeprom.header.data_version < 1) {
		error("EEPROM header has invalid data-version: %u (actual) < 1 (expected)", u.eeprom.header.data_version);

		return;
	}

	if (u.eeprom.header.data_version == 1 && u.eeprom.header.data_length < sizeof(u.eeprom.data_v1)) {
		error("EEPROM header has invalid data-length: %u (actual) < %u (expected)", u.eeprom.header.data_length, sizeof(u.eeprom.data_v1));

		return;
	}

	if (u.eeprom.data_v1.uid[sizeof(u.eeprom.data_v1.uid) - 1] != '\0') {
		error("EEPROM data UID is not null-terminated");

		return;
	}

	if (u.eeprom.data_v1.hostname[sizeof(u.eeprom.data_v1.hostname) - 1] != '\0') {
		error("EEPROM data hostname is not null-terminated");

		return;
	}

	if (u.eeprom.data_v1.encrypted_password[sizeof(u.eeprom.data_v1.encrypted_password) - 1] != '\0') {
		error("EEPROM data encrypted-password is not null-terminated");

		return;
	}

	memcpy(&eeprom, &u.eeprom, sizeof(eeprom));

	eeprom_valid = true;
}

static void replace_password(void)
{
	int fd;
	struct stat st;
	char *buffer;
	size_t buffer_used;
	ssize_t length;
	char *entry_begin;
	char *encrypted_begin;
	char *encrypted_end;
	char encrypted[SHADOW_ENCRYPTED_LENGTH];
	size_t encrypted_used;
	char salt[SHADOW_ENCRYPTED_LENGTH]; // over-allocate to be safe
	size_t salt_used;
	char *encrypted_prefix_end;
	struct crypt_data crypt_data;
	const char *crypt_result;

	if (!eeprom_valid || eeprom.header.data_version < 1) {
		error("required EEPROM data not available, skipping password replacement");

		return;
	}

	crypt_data.initialized = 0;

	// open /etc/shadow
	print("opening %s", SHADOW_PATH);

	fd = open(SHADOW_PATH, O_RDONLY);

	if (fd < 0) {
		panic("could not open %s for reading: %s (%d)", SHADOW_PATH, strerror(errno), errno);
	}

	if (fstat(fd, &st) < 0) {
		panic("could not get status of %s: %s (%d)", SHADOW_PATH, strerror(errno), errno);
	}

	if (st.st_size > SHADOW_BUFFER_LENGTH) {
		panic("%s is too big", SHADOW_PATH);
	}

	// read /etc/shadow
	print("reading %s", SHADOW_PATH);

	buffer_used = st.st_size;
	buffer = malloc(buffer_used + 1); // +1 for null-terminator

	if (buffer == NULL) {
		panic("could not allocate memory");
	}

	length = read(fd, buffer, buffer_used);

	if (length < 0) {
		panic("could not read from %s: %s (%d)", SHADOW_PATH, strerror(errno), errno);
	}

	if ((size_t)length < buffer_used) {
		panic("short read from %s: %s (%d)", SHADOW_PATH, strerror(errno), errno);
	}

	buffer[buffer_used] = '\0';

	print("closing %s", SHADOW_PATH);

	close(fd);

	// find entry for account
	if (strncmp(buffer, ACCOUNT_NAME":", strlen(ACCOUNT_NAME":")) == 0) {
		entry_begin = buffer;
	} else {
		entry_begin = strstr(buffer, "\n"ACCOUNT_NAME":");

		if (entry_begin == NULL) {
			print("account %s is not present, skipping password replacement", ACCOUNT_NAME);

			goto cleanup;
		}

		++entry_begin; // skip new-line
	}

	// find encrypted section in entry
	encrypted_begin = strchr(entry_begin, ':');

	if (encrypted_begin == NULL) {
		panic("encrypted section for account %s is malformed", ACCOUNT_NAME);
	}

	++encrypted_begin; // skip colon

	if (encrypted_begin[0] == '*') {
		print("account %s has no password set, skipping password replacement", ACCOUNT_NAME);

		goto cleanup;
	}

	if (encrypted_begin[0] != '!') {
		print("account %s is not locked, skipping password replacement", ACCOUNT_NAME);

		goto cleanup;
	}

	encrypted_end = strchr(encrypted_begin, ':');

	if (encrypted_end == NULL) {
		panic("encrypted section for account %s is malformed", ACCOUNT_NAME);
	}

	encrypted_used = encrypted_end - (encrypted_begin + 1); // +1 to skip exclamation mark

	if (encrypted_used > SHADOW_ENCRYPTED_LENGTH) {
		panic("encrypted section for account %s is too big", ACCOUNT_NAME);
	}

	memcpy(encrypted, encrypted_begin + 1, encrypted_used); // +1 to skip exclamation mark

	encrypted[encrypted_used] = '\0';

	// get salt from encrypted section
	if (encrypted_used < 2) {
		panic("encrypted section for account %s is malformed", ACCOUNT_NAME);
	}

	if (encrypted[0] != '$') {
		salt_used = 2;
	} else {
		encrypted_prefix_end = strrchr(encrypted, '$');

		if (encrypted_prefix_end == NULL) {
			panic("encrypted section for account %s is malformed", ACCOUNT_NAME);
		}

		salt_used = encrypted_prefix_end - encrypted;
	}

	memcpy(salt, encrypted, salt_used);

	salt[salt_used] = '\0';

	// encrypt default password with salt from encrypted section
	crypt_result = crypt_r(DEFAULT_PASSWORD, salt, &crypt_data);

	if (crypt_result == NULL) {
		panic("could not encrypt default password: %s (%d)", strerror(errno), errno);
	}

	if (strcmp(crypt_result, encrypted) != 0) {
		print("account %s does not have the default password set, skipping password replacement", ACCOUNT_NAME);

		goto cleanup;
	}

	print("account %s has default password set, replacing with device specific password", ACCOUNT_NAME);

	// create /etc/shadow-
	fd = create_file(SHADOW_BACKUP_PATH, st.st_uid, st.st_gid, st.st_mode);

	robust_write(SHADOW_BACKUP_PATH, fd, buffer, buffer_used);

	print("closing %s", SHADOW_BACKUP_PATH);

	fsync(fd);
	close(fd);

	// create /etc/shadow+
	fd = create_file(SHADOW_TMP_PATH, st.st_uid, st.st_gid, st.st_mode);

	robust_write(SHADOW_TMP_PATH, fd, buffer, encrypted_begin - buffer);
	robust_write(SHADOW_TMP_PATH, fd, eeprom.data_v1.encrypted_password, strlen(eeprom.data_v1.encrypted_password));
	robust_write(SHADOW_TMP_PATH, fd, encrypted_end, buffer_used - (encrypted_end - buffer));

	print("closing %s", SHADOW_TMP_PATH);

	fsync(fd);
	close(fd);

	// rename /etc/shadow+ to /etc/shadow
	print("renaming %s to %s", SHADOW_TMP_PATH, SHADOW_PATH);

	if (rename(SHADOW_TMP_PATH, SHADOW_PATH) < 0) {
		panic("could not rename %s to %s: %s (%d)", SHADOW_TMP_PATH, SHADOW_PATH, strerror(errno), errno);
	}

cleanup:
	free(buffer);
}

static void configure_ethernet(void)
{
	DIR *dp;
	struct dirent *dirent;
	int fd;
	struct ifreq ifr;
	union {
		struct ethtool_eeprom eeprom;
		uint8_t bytes[sizeof(struct ethtool_eeprom) + ETHERNET_CONFIG_LENGTH];
	} u;

	if (!eeprom_valid || eeprom.header.data_version < 1) {
		error("required EEPROM data not available, skipping Ethernet configuration");

		return;
	}

	// find Ethernet device name
	print("looking up Ethernet device name");

	dp = opendir(ETHERNET_DEVICE_PATH"net/");

	if (dp == NULL) {
		panic("could not open net/ subdirectory of Ethernet device %s: %s (%d)",
		      ETHERNET_DEVICE_PATH, strerror(errno), errno);
	}

	errno = 0;
	dirent = readdir(dp);

	if (dirent == NULL) {
		panic("could not read net/ subdirectory of Ethernet device %s: %s (%d)",
		      ETHERNET_DEVICE_PATH, strerror(errno), errno);
	}

	if (dirent->d_type != DT_DIR) {
		panic("directory entry %s of %snet/ has unexpected type: %d",
		      dirent->d_name, ETHERNET_DEVICE_PATH, dirent->d_type);
	}

	if (strlen(dirent->d_name) >= IFNAMSIZ) {
		panic("Ethernet device name %s is too long: %u > %u",
		      dirent->d_name, strlen(dirent->d_name), IFNAMSIZ - 1);
	}

	print("found Ethernet device name: %s", dirent->d_name);

	memset(&ifr, 0, sizeof(ifr));
	memcpy(ifr.ifr_name, dirent->d_name, IFNAMSIZ);

	closedir(dp);

	ifr.ifr_name[IFNAMSIZ - 1] = '\0';
	ifr.ifr_data = (char *)&u.eeprom;

	// open control socket
	print("opening ethtool control socket");

	fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0) {
		fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);

		if (fd < 0) {
			panic("could not open ethtool control socket: %s (%d)", strerror(errno), errno);
		}
	}

	// check if config is already set
	print("reading first Ethernet config byte");

	memset(&u.bytes, 0, sizeof(u.bytes));

	u.eeprom.cmd = ETHTOOL_GEEPROM;
	u.eeprom.len = 1;
	u.eeprom.offset = 0;

	if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) {
		panic("could not read first Ethernet config byte: %s (%d)", strerror(errno), errno);
	}

	if (u.eeprom.data[0] == 0xA5) {
		print("Ethernet already configured, skipping Ethernet configuration");
		close(fd);

		return;
	}

	// write config
	print("writing Ethernet config");

	memset(&u.bytes, 0, sizeof(u.bytes));

	u.eeprom.cmd = ETHTOOL_SEEPROM;
	u.eeprom.len = ETHERNET_CONFIG_LENGTH;
	u.eeprom.offset = 0;
	u.eeprom.magic = 0x7500;

	memcpy(u.eeprom.data, eeprom.data_v1.ethernet_config, ETHERNET_CONFIG_LENGTH);

	if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) {
		panic("could not write Ethernet config: %s (%d)", strerror(errno), errno);
	}

	usleep(100 * 1000); // wait 100 msec to let everything settle

	// validate config
	print("validating Ethernet config");

	memset(&u.bytes, 0, sizeof(u.bytes));

	u.eeprom.cmd = ETHTOOL_GEEPROM;
	u.eeprom.len = ETHERNET_CONFIG_LENGTH;
	u.eeprom.offset = 0;

	if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) {
		panic("could not read Ethernet config: %s (%d)", strerror(errno), errno);
	}

	if (memcmp(u.eeprom.data, eeprom.data_v1.ethernet_config, ETHERNET_CONFIG_LENGTH) != 0) {
		panic("Ethernet config validation failed");
	}

	close(fd);
}

void update_file(const char *path, void *content, size_t content_length)
{
	struct stat st;
	int fd;
	char buffer[content_length];
	ssize_t length;
	char tmp_path[256];

	if (stat(path, &st) < 0) {
		if (errno != ENOENT) {
			error("could not get status of %s: %s (%d)", path, strerror(errno), errno);
		}

		goto update;
	} else if (st.st_mode != (S_IFREG | 0444) || st.st_uid != 0 || st.st_gid != 0 ||
	           (size_t)st.st_size != content_length) {
		goto update;
	}

	fd = open(path, O_RDONLY);

	if (fd < 0) {
		error("could not open %s for reading: %s (%d)", path, strerror(errno), errno);

		goto update;
	}

	length = read(fd, buffer, st.st_size);

	close(fd);

	if (length < 0) {
		error("could not read from %s: %s (%d)", path, strerror(errno), errno);

		goto update;
	}

	if (length < st.st_size) {
		error("short read from %s: %s (%d)", path, strerror(errno), errno);

		goto update;
	}

	if (memcmp(buffer, content, content_length) == 0) {
		print("%s is already up-to-date, skipping update", path);

		return;
	}

update:
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

	fd = create_file(tmp_path, 0, 0, 0444);

	robust_write(tmp_path, fd, content, content_length);

	fsync(fd);
	close(fd);

	print("renaming %s to %s", tmp_path, path);

	if (rename(tmp_path, path) < 0) {
		panic("could not rename %s to %s: %s (%d)", tmp_path, path, strerror(errno), errno);
	}
}

static void read_cmdline(const char **root, const char **rootfstype, const char **init)
{
	int fd;
	static char buffer[2048];
	ssize_t length;
	char *option;

	*root = NULL;
	*rootfstype = NULL;
	*init = NULL;

	print("reading /proc/cmdline");

	// read /proc/cmdline
	fd = open("/proc/cmdline", O_RDONLY);

	if (fd < 0) {
		panic("could not open /proc/cmdline for reading: %s (%d)", strerror(errno), errno);
	}

	length = read(fd, buffer, sizeof(buffer) - 1);

	if (length < 0) {
		panic("could not read from /proc/cmdline: %s (%d)", strerror(errno), errno);
	}

	close(fd);

	buffer[length] = '\0';

	// parse /proc/cmdline
	option = strtok(buffer, "\r\n\t ");

	while (option != NULL) {
		if (strncmp(option, "root=", 5) == 0) {
			*root = option + 5;
		} else if (strncmp(option, "rootfstype=", 11) == 0) {
			*rootfstype = option + 11;
		} else if (strncmp(option, "init=", 5) == 0) {
			*init = option + 5;
		}

		option = strtok(NULL, "\r\n\t ");
	}
}

int main(void)
{
	const char *root;
	const char *rootfstype;
	const char *init;
	char buffer[256];
	const char *execv_argv[] = {NULL, NULL};

	// open /dev/kmsg
	kmsg_fd = open("/dev/kmsg", O_WRONLY);

	// mount /proc
	print("mounting proc at /proc");

	if (mount("proc", "/proc", "proc", 0, "") < 0) {
		panic("could not mount proc at /proc: %s (%d)", strerror(errno), errno);
	}

	// read cmdline
	read_cmdline(&root, &rootfstype, &init);

	if (root == NULL) {
		root = "/dev/mmcblk0p2";
	}

	if (rootfstype == NULL) {
		rootfstype = "ext4";
	}

	if (init == NULL) {
		init = "/sbin/init";
	}

	// mount /sys
	print("mounting sysfs at /sys");

	if (mount("sysfs", "/sys", "sysfs", 0, "") < 0) {
		panic("could not mount sysfs at /sys: %s (%d)", strerror(errno), errno);
	}

	// mount /dev
	print("mounting devtmpfs at /dev");

	if (mount("devtmpfs", "/dev", "devtmpfs", 0, "") < 0) {
		panic("could not mount devtmpfs at /dev: %s (%d)", strerror(errno), errno);
	}

	// wait 250 msec for the root device to show up before trying to mount it to
	// avoid an initial warning about the device not being available yet
	usleep(250 * 1000);

	// mount root at /mnt
	robust_mount(root, "/mnt", rootfstype, MS_NOATIME);

	// mount devtmpfs at /mnt/dev
	print("mounting devtmpfs at /mnt/dev");

	if (mount("devtmpfs", "/mnt/dev", "devtmpfs", 0, "") < 0) {
		panic("could not mount devtmpfs at /mnt/dev: %s (%d)", strerror(errno), errno);
	}

	// set system clock from RTC
	modprobe("i2c_bcm2835");
	modprobe("rtc_pcf8523");
	rtc_hctosys();

	// read eeprom content
	modprobe("i2c_dev");
	read_eeprom();

	// replace password if necessary
	replace_password();

	// configure Ethernet if necessary
	configure_ethernet();

	// write /etc/tng-base-* files
	if (!eeprom_valid || eeprom.header.data_version < 1) {
		error("required EEPROM data not available, skip updating /mnt/etc/tng-base-* files");
	} else {
		print("updating /mnt/etc/tng-base-* files");

		// production date
		snprintf(buffer, sizeof(buffer), "%04X-%02X-%02X\n",
		         eeprom.data_v1.production_date >> 16,
		         (eeprom.data_v1.production_date >> 8) & 0xFF,
		         eeprom.data_v1.production_date & 0xFF);

		update_file("/mnt/etc/tng-base-production-date", buffer, strlen(buffer));

		// UID
		snprintf(buffer, sizeof(buffer), "%s\n", eeprom.data_v1.uid);

		update_file("/mnt/etc/tng-base-uid", buffer, strlen(buffer));

		// hostname
		snprintf(buffer, sizeof(buffer), "%s\n", eeprom.data_v1.hostname);

		update_file("/mnt/etc/tng-base-hostname", buffer, strlen(buffer));
	}

	// unmount /proc
	print("unmounting /proc");

	if (umount("/proc") < 0) {
		panic("could not unmount /proc: %s (%d)", strerror(errno), errno);
	}

	// unmount /sys
	print("unmounting /sys");

	if (umount("/sys") < 0) {
		panic("could not unmount /sys: %s (%d)", strerror(errno), errno);
	}

	// unmount /dev
	print("unmounting /dev");

	if (umount("/dev") < 0) {
		panic("could not unmount /dev: %s (%d)", strerror(errno), errno);
	}

	// switch root (logic taken from busybox switch_root and simplified)
	print("switching root-mount to /mnt");

	if (chdir("/mnt") < 0) {
		panic("could not change current directory to /mnt: %s (%d)", strerror(errno), errno);
	}

	unlink("/init"); // unlink ourself to free some memory

	if (mount(".", "/", NULL, MS_MOVE, NULL) < 0) {
		panic("could not move root-mount: %s (%d)", strerror(errno), errno);
	}

	if (chroot(".") < 0) {
		panic("could not chroot into /mnt: %s (%d)", strerror(errno), errno);
	}

	if (chdir("/") < 0) {
		panic("could not change current directory to /: %s (%d)", strerror(errno), errno);
	}

	// execute /sbin/init
	print("executing %s in /mnt", init);

	if (kmsg_fd >= 0) {
		close(kmsg_fd);

		kmsg_fd = -1;
	}

	execv_argv[0] = init;

	execv(execv_argv[0], (char **)execv_argv);

	panic("could not execute %s in /mnt: %s (%d)", init, strerror(errno), errno);

	return EXIT_FAILURE; // unreachable
}
