/*
 *     Copyright (C) 2010-2016 Marvell International Ltd.
 *     Copyright (C) 2002-2010 Kinoma, Inc.
 *
 *     Licensed under the Apache License, Version 2.0 (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include "FskPin.h"
#include "FskMemory.h"
#include "FskThread.h"

#if TARGET_OS_LINUX
	#define USE_FD 1
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <termios.h>
#include <fcntl.h>
#if USE_FD
	#include <sys/eventfd.h>
#else
	#include <fcntl.h>
	#include <unistd.h>
#endif
#include <errno.h>
#include <sys/ioctl.h>
#include <poll.h>

#include <sys/socket.h>
#define BTPROTO_HCI			1
#define HCI_CHANNEL_USER	1
struct sockaddr_hci {
	sa_family_t		hci_family;
	unsigned short	hci_dev;
	unsigned short	hci_channel;
};

static Boolean linuxSerialCanHandle(SInt32 rxNumber, SInt32 txNumber, const char *name, char **remappedName);
static FskErr linuxSerialNew(FskPinSerial *pin, SInt32 rxNumber, SInt32 txNumber, const char *name, SInt32 baud);
static void linuxSerialDispose(FskPinSerial pin);
static FskErr linuxSerialWrite(FskPinSerial pin, SInt32 bufferSize, const UInt8 *bytes);
static FskErr linuxSerialRead(FskPinSerial pin, SInt32 bufferSize, SInt32 *bytesRead, UInt8 *bytes);
static FskErr linuxSerialDataReady(FskPinSerial pin, FskPinSerialRepeatTriggerProc triggered, void *refCon);

FskPinSerialDispatchRecord gLinuxPinSerial = {
	linuxSerialCanHandle,
	linuxSerialNew,
	linuxSerialDispose,
	linuxSerialWrite,
	linuxSerialRead,
	linuxSerialDataReady
};

typedef struct {
	FskPinSerialRecord				pd;

	int								ttyFile;

	FskPinSerialRepeatTriggerProc	triggered;
	void							*refCon;

	FskThread						serialThread;
	FskMutex						mutex;
#if USE_FD
	int								eventfd;
#else
	int								pipe[2];
#endif
	Boolean							killed;

	unsigned char					*data;
	UInt32							dataCount;		// data bytes available
	UInt32							dataAllocated;	// size of data
} linuxSerialRecord, *linuxSerial;

static speed_t getBaud(int baud);
static void disposeSerialThread(linuxSerial ls);
static void serialThread(void *param);

Boolean linuxSerialCanHandle(SInt32 rxNumber, SInt32 txNumber, const char *name, char **remappedName)
{
	return NULL != name;
}

FskErr linuxSerialNew(FskPinSerial *pin, SInt32 rxNumber, SInt32 txNumber, const char *name, SInt32 baud)
{
	FskErr err;
	linuxSerial ls;
	int result;
	speed_t speed = getBaud((int)baud);
	struct termios config;

	err = FskMemPtrNewClear(sizeof(linuxSerialRecord), &ls);
	if (err) return err;

#if USE_FD
	ls->eventfd = -1;
#else
	ls->pipe[0] = ls->pipe[1] = -1;
#endif

	err = FskMutexNew(&ls->mutex, "serial pin");
	if (err) goto bail;

	if (strncmp(name, "hci", 3) == 0) {
		struct sockaddr_hci addr;

		ls->ttyFile = socket(AF_BLUETOOTH, SOCK_RAW /*| SOCK_CLOEXEC*/, BTPROTO_HCI);
		if (ls->ttyFile == -1)
			BAIL(kFskErrOperationFailed);

		memset(&addr, 0, sizeof(addr));
		addr.hci_family = AF_BLUETOOTH;
		addr.hci_dev = atoi(name + 3);
		addr.hci_channel = HCI_CHANNEL_USER;
		if (bind(ls->ttyFile, (struct sockaddr *) &addr, sizeof(addr)) == -1)
			BAIL(kFskErrOperationFailed);

		err = kFskErrNone;
		goto bail;
	}

	ls->ttyFile = open(name, O_RDWR | O_NOCTTY | O_NDELAY | O_NONBLOCK);
	if (ls->ttyFile < 0)
		BAIL(kFskErrOperationFailed);

	result = fcntl(ls->ttyFile, F_SETFL, O_RDWR);
	if (-1 == result)
		BAIL(kFskErrUnknown);
	tcgetattr(ls->ttyFile, &config);

	cfmakeraw(&config);

	config.c_cflag |= CREAD | CLOCAL;
	config.c_cflag &= ~(CSTOPB);

	result = cfsetospeed(&config, speed);
	result += cfsetispeed(&config, speed);
	if (result < 0)
		BAIL(kFskErrInvalidParameter);

	config.c_cc[VTIME] = 0;
	config.c_cc[VMIN] = 1;

	tcsetattr(ls->ttyFile, TCSAFLUSH, &config);

bail:
	if (err) {
		linuxSerialDispose((FskPinSerial)ls);
		ls = NULL;
	}
	*pin = (FskPinSerial)ls;

	return err;
}

void linuxSerialDispose(FskPinSerial pin)
{
	linuxSerial ls = (linuxSerial)pin;

	disposeSerialThread(ls);

	if (ls->ttyFile >= 0)
		close(ls->ttyFile);

	FskMemPtrDispose(ls->mutex);

	FskMemPtrDispose(ls);
}

FskErr linuxSerialWrite(FskPinSerial pin, SInt32 bufferSize, const UInt8 *bytes)
{
	linuxSerial ls = (linuxSerial)pin;
	int result = write(ls->ttyFile, bytes, bufferSize);
	return (result == bufferSize) ? kFskErrNone : kFskErrOperationFailed;
}

FskErr linuxSerialRead(FskPinSerial pin, SInt32 bufferSize, SInt32 *bytesReadOut, UInt8 *bytes)
{
	linuxSerial ls = (linuxSerial)pin;
	int bytesAvailable, bytesRead;
	int result;

	if (ls->serialThread) {
		FskMutexAcquire(ls->mutex);

		if (ls->dataCount) {
			bytesAvailable = ls->dataCount;
			if (bytesAvailable > bufferSize)
				bytesAvailable = bufferSize;

			FskMemMove(bytes, ls->data, bytesAvailable);
			*bytesReadOut = bytesAvailable;

			FskMemMove(ls->data, ls->data + bytesAvailable, ls->dataCount - bytesAvailable);
			ls->dataCount -= bytesAvailable;

			FskMutexRelease(ls->mutex);
		}
		else {
			*bytesReadOut = 0;

			FskMutexRelease(ls->mutex);

			return kFskErrNoData;
		}

		return kFskErrNone;
	}

	result = ioctl(ls->ttyFile, FIONREAD, &bytesAvailable);
	if ((bytesAvailable <= 0) || (result < 0))
		return kFskErrNoData;

	bytesRead = read(ls->ttyFile, bytes, (bytesAvailable > bufferSize) ? bufferSize : bytesAvailable);
	if (bytesRead <= 0)
		return kFskErrOperationFailed;

	*bytesReadOut = (SInt32)bytesRead;

	return kFskErrNone;
}

FskErr linuxSerialDataReady(FskPinSerial pin, FskPinSerialRepeatTriggerProc triggered, void *refCon)
{
	linuxSerial ls = (linuxSerial)pin;
	FskErr err = kFskErrNone;

	if (NULL == triggered) {
		disposeSerialThread(ls);
		ls->triggered = NULL;
		return kFskErrNone;
	}

	ls->triggered = triggered;
	ls->refCon = refCon;

	if (NULL == ls->serialThread) {
#if USE_FD
		ls->eventfd = eventfd(0, EFD_NONBLOCK);
		if (ls->eventfd >= 0) {
#else
		if (0 == pipe(ls->pipe)) {
			fcntl(ls->pipe[0], O_NONBLOCK);
#endif
			err = FskThreadCreate(&ls->serialThread, serialThread, kFskThreadFlagsJoinable, ls, "serial poller");
			if (err) {
#if USE_FD
				close(ls->eventfd);
				ls->eventfd = -1;
#else
				close(ls->pipe[0]);
				close(ls->pipe[1]);
				ls->pipe[0] = ls->pipe[1] = -1;
#endif
			}
		}
	}

	return err;
}

speed_t getBaud(int baud)
{
	switch (baud) {
		case 0:
			return B0;
		case 50:
			return B50;
		case 75:
			return B75;
		case 110:
			return B110;
		case 134:
			return B134;
		case 150:
			return B150;
		case 200:
			return B200;
		case 300:
			return B300;
		case 600:
			return B600;
		case 1200:
			return B1200;
		case 1800:
			return B1800;
		case 2400:
			return B2400;
		case 4800:
			return B4800;
		case 9600:
			return B9600;
		case 19200:
			return B19200;
		case 38400:
			return B38400;
		case 57600:
			return B57600;
		case 115200:
			return B115200;
		case 230400:
			return B230400;
#if TARGET_OS_LINUX
		case 460800:
			return B460800;
		case 500000:
			return B500000;
		case 576000:
			return B576000;
		case 921600:
			return B921600;
		case 1000000:
			return B1000000;
		case 1152000:
			return B1152000;
		case 1500000:
			return B1500000;
		case 2000000:
			return B2000000;
		case 2500000:
			return B2500000;
		case 3000000:
			return B3000000;
		case 3500000:
			return B3500000;
		case 4000000:
			return B4000000;
#endif
		default:
			return B0;
	}
}

void serialThread(void *param)
{
    linuxSerial ls = param;
	unsigned char buffer[1024];
	struct pollfd fds[2];

#if USE_FD
	fds[0].fd = ls->eventfd;
	fds[0].events = POLLIN | POLLERR | POLLHUP;
#else
	fds[0].fd = ls->pipe[0];
	fds[0].events = POLLIN | POLLERR;
#endif

	fds[1].fd = ls->ttyFile;
	fds[1].events = POLLIN | POLLERR;

	FskThreadInitializationComplete(FskThreadGetCurrent());
	while (!ls->killed) {
		int bytesRead;

		poll(fds, 2, -1);

		if (!(fds[1].revents & POLLIN))
			continue;

		bytesRead = read(ls->ttyFile, buffer, sizeof(buffer));
		if (bytesRead < 0)
			break;

		FskMutexAcquire(ls->mutex);

		if ((bytesRead + ls->dataCount) > ls->dataAllocated) {
			UInt32 newSize = (bytesRead + ls->dataCount) * 2;
			if (newSize < 8192) newSize = 8192;
			if (kFskErrNone != FskMemPtrRealloc(newSize, &ls->data)) {
				FskMutexRelease(ls->mutex);
				break;
			}
			ls->dataAllocated = newSize;
		}

		FskMemMove(ls->data + ls->dataCount, buffer, bytesRead);
		ls->dataCount += bytesRead;

		FskMutexRelease(ls->mutex);

		(ls->triggered)((FskPinSerial)ls, ls->refCon);
	}
}

void disposeSerialThread(linuxSerial ls)
{
	if (ls->serialThread) {
		uint64_t one = 1;
		ls->killed = true;
#if USE_FD
		if (ls->eventfd >= 0)
			write(ls->eventfd, &one, sizeof(one));
#else
		if (ls->pipe[1] >= 0)
			write(ls->pipe[1], &one, sizeof(one));
#endif
		FskThreadJoin(ls->serialThread);
#if USE_FD
		if (ls->eventfd >= 0)
			close(ls->eventfd);
#else
		if (ls->pipe[0] >= 0)
			close(ls->pipe[0]);
		if (ls->pipe[1] >= 0)
			close(ls->pipe[1]);
#endif
		FskMutexAcquire(ls->mutex);
			FskMemPtrDisposeAt(&ls->data);
			ls->dataAllocated = 0;
			ls->dataCount = 0;
			ls->serialThread = NULL;
			ls->killed = false;
#if USE_FD
			ls->eventfd = -1;
#else
			ls->pipe[0] = ls->pipe[1] = -1;
#endif
		FskMutexRelease(ls->mutex);
	}
}

/*
	Extension
*/

FskExport(FskErr) FskPinSerialLinux_fskLoad(FskLibrary library)
{
	return FskExtensionInstall(kFskExtensionPinSerial, &gLinuxPinSerial);
}

FskExport(FskErr) FskPinSerialLinux_fskUnload(FskLibrary library)
{
	return FskExtensionUninstall(kFskExtensionPinSerial, &gLinuxPinSerial);
}
