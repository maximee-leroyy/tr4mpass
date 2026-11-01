/*
 * pongo_upload.c -- talk to the live pongoOS shell: upload a file into
 * loader_xfer_recv_data and/or run a shell command.
 *
 * Protocol (pongoOS src/shell/usbloader.c, mineek/PongoOS master):
 *   - bmRequestType 0x21, bRequest 1, wLength 0 : init bulk upload on
 *     EP2 OUT (device preps usb_out_transfer_dma into
 *     loader_xfer_recv_data)
 *   - bulk OUT (EP2, interface 0) : the payload bytes
 *   - bmRequestType 0x21, bRequest 3, wLength N : write N bytes to the
 *     pongo shell stdin (e.g. "ramdisk\n")
 *   - bmRequestType 0xA1, bRequest 3 (stdout reads are via the interrupt
 *     IN pipe; not needed for fire-and-forget commands)
 *
 * Transport: IOKit IOUSBDeviceInterface (device user client) --
 * DeviceRequestTO for control, WritePipe for the bulk upload. This is
 * the same user client pongoterm uses (FoundDevice in pongoterm.c).
 *
 * Usage:
 *   pongo_upload upload <file>      upload file into the xfer buffer
 *   pongo_upload cmd "<command>"    run one shell command
 *   pongo_upload run <file> <cmd>   upload then run command
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <CoreFoundation/CoreFoundation.h>

#define PONGO_VID 0x05AC
#define PONGO_PID 0x4141

static IOUSBDeviceInterface245 **g_dev;
static IOUSBInterfaceInterface245 **g_ifc;

static io_service_t find_pongo(void)
{
	CFMutableDictionaryRef dict =
	    IOServiceMatching(kIOUSBDeviceClassName);
	if (!dict)
		return IO_OBJECT_NULL;
	CFNumberRef vid = CFNumberCreate(NULL, kCFNumberSInt16Type,
	                                 &(SInt16){PONGO_VID});
	CFNumberRef pid = CFNumberCreate(NULL, kCFNumberSInt16Type,
	                                 &(SInt16){PONGO_PID});
	CFDictionarySetValue(dict, CFSTR(kUSBVendorID), vid);
	CFDictionarySetValue(dict, CFSTR(kUSBProductID), pid);
	CFRelease(vid);
	CFRelease(pid);

	io_iterator_t iter = IO_OBJECT_NULL;
	if (IOServiceGetMatchingServices(kIOMainPortDefault, dict, &iter)
	    != KERN_SUCCESS)
		return IO_OBJECT_NULL;
	io_service_t serv = IOIteratorNext(iter);
	IOObjectRelease(iter);
	return serv;
}

static int open_pongo(void)
{
	io_service_t serv = find_pongo();
	if (!serv) {
		fprintf(stderr, "pongoOS device not found (05ac:4141)\n");
		return 1;
	}
	IOCFPlugInInterface **plug = NULL;
	SInt32 score = 0;
	if (IOCreatePlugInInterfaceForService(
	        serv, kIOUSBDeviceUserClientTypeID,
	        kIOCFPlugInInterfaceID, &plug, &score) != KERN_SUCCESS) {
		fprintf(stderr, "plug-in creation failed\n");
		return 1;
	}
	HRESULT hr = (*plug)->QueryInterface(
	    plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID245),
	    (LPVOID *)&g_dev);
	(*plug)->Release(plug);
	IOObjectRelease(serv);
	if (hr != 0) {
		fprintf(stderr, "QueryInterface failed 0x%x\n", hr);
		return 1;
	}
	IOReturn r = (*g_dev)->USBDeviceOpenSeize(g_dev);
	if (r != KERN_SUCCESS) {
		fprintf(stderr, "USBDeviceOpenSeize: 0x%x\n", r);
		return 1;
	}
	r = (*g_dev)->SetConfiguration(g_dev, 1);
	if (r != KERN_SUCCESS)
		fprintf(stderr, "SetConfiguration: 0x%x (continuing)\n", r);

	/* open interface 0 (pongoterm opens the first interface and uses
	 * it for both control and the EP2 bulk pipe) */
	IOUSBFindInterfaceRequest ifreq = {
		.bInterfaceClass = kIOUSBFindInterfaceDontCare,
		.bInterfaceSubClass = kIOUSBFindInterfaceDontCare,
		.bInterfaceProtocol = kIOUSBFindInterfaceDontCare,
		.bAlternateSetting = kIOUSBFindInterfaceDontCare,
	};
	io_iterator_t iter = IO_OBJECT_NULL;
	r = (*g_dev)->CreateInterfaceIterator(g_dev, &ifreq, &iter);
	if (r != KERN_SUCCESS) {
		fprintf(stderr, "CreateInterfaceIterator: 0x%x\n", r);
		return 1;
	}
	io_service_t ifsrv = IOIteratorNext(iter);
	IOObjectRelease(iter);
	if (!ifsrv) {
		fprintf(stderr, "no interface\n");
		return 1;
	}
	IOCFPlugInInterface **iplug = NULL;
	SInt32 iscore = 0;
	r = IOCreatePlugInInterfaceForService(
	    ifsrv, kIOUSBInterfaceUserClientTypeID,
	    kIOCFPlugInInterfaceID, &iplug, &iscore);
	IOObjectRelease(ifsrv);
	if (r != KERN_SUCCESS) {
		fprintf(stderr, "interface plug-in failed\n");
		return 1;
	}
	HRESULT ihr = (*iplug)->QueryInterface(
	    iplug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID245),
	    (LPVOID *)&g_ifc);
	(*iplug)->Release(iplug);
	if (ihr != 0) {
		fprintf(stderr, "interface QueryInterface 0x%x\n", ihr);
		return 1;
	}
	r = (*g_ifc)->USBInterfaceOpen(g_ifc);
	if (r != KERN_SUCCESS) {
		fprintf(stderr, "USBInterfaceOpen: 0x%x\n", r);
		return 1;
	}
	return 0;
}

static void close_pongo(void)
{
	(*g_ifc)->USBInterfaceClose(g_ifc);
	(*g_ifc)->Release(g_ifc);
	(*g_dev)->USBDeviceClose(g_dev);
	(*g_dev)->Release(g_dev);
}

static IOUSBInterfaceInterface245 **g_ifc;

/* Control transfers go through the INTERFACE user client, mirroring
 * pongoterm (USBControlTransfer there = ControlRequest on
 * IOUSBInterfaceInterface245).  E9/E28: on M-series the device user
 * client stalls where the interface client survives. */
static int ctrl(uint8_t bm, uint8_t req, uint16_t wv, uint16_t wi,
                void *data, uint16_t len)
{
	IOUSBDevRequest request = {
		.bmRequestType = bm,
		.bRequest = req,
		.wValue = wv,
		.wIndex = wi,
		.wLength = len,
		.pData = data,
	};
	IOReturn r = (*g_ifc)->ControlRequest(g_ifc, 0, &request);
	if (r != KERN_SUCCESS) {
		fprintf(stderr, "ctrl 0x%02x/%d: 0x%x\n", bm, req, r);
		return 1;
	}
	return 0;
}

static int ifc_reopen(void)
{
	(*g_ifc)->USBInterfaceClose(g_ifc);
	IOReturn r = (*g_ifc)->USBInterfaceOpen(g_ifc);
	if (r != KERN_SUCCESS) {
		fprintf(stderr, "USBInterfaceOpen (recover): 0x%x\n", r);
		return 1;
	}
	return 0;
}

static int bulk_write(const uint8_t *buf, long sz)
{
	/* 16 KiB chunks with a no-data timeout: near the end of a big DMA
	 * WritePipe can block indefinitely (seen at 99% of 105 MB) while a
	 * fresh write succeeds -- recover via stall-clear + interface
	 * reopen, then push the remaining bytes. */
	long off = 0;
	long next_report = 0;
	while (off < sz) {
		long chunk = (sz - off > 16384) ? 16384 : (sz - off);
		IOReturn r = (*g_ifc)->WritePipeTO(g_ifc, 2,
		                                   (void *)(buf + off),
		                                   (UInt32)chunk, 5000, 0);
		if (r != KERN_SUCCESS) {
			fprintf(stderr, "\nWritePipe at %ld: 0x%x -- "
			        "recovering\n", off, r);
			(*g_ifc)->ClearPipeStallBothEnds(g_ifc, 2);
			r = (*g_ifc)->WritePipeTO(g_ifc, 2,
			                          (void *)(buf + off),
			                          (UInt32)chunk, 5000, 0);
			if (r != KERN_SUCCESS && !ifc_reopen()) {
				r = (*g_ifc)->WritePipeTO(g_ifc, 2,
				                          (void *)(buf + off),
				                          (UInt32)chunk,
				                          5000, 0);
			}
			if (r != KERN_SUCCESS) {
				fprintf(stderr,
				        "WritePipe retry at %ld: 0x%x\n",
				        off, r);
				return 1;
			}
		}
		off += chunk;
		if (off >= next_report) {
			fprintf(stderr, "\r%ld/%ld bytes (%ld%%)", off, sz,
			        off * 100 / sz);
			next_report = off + (4 << 20);
		}
	}
	fprintf(stderr, "\n");
	return 0;
}

static int upload(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror("fopen");
		return 1;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc((size_t)sz);
	if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		fprintf(stderr, "read failed\n");
		fclose(f);
		return 1;
	}
	fclose(f);
	printf("file: %s (%ld bytes)\n", path, sz);

	/* init bulk upload: 0x21/1 with a 4-byte size data phase
	 * (reallocate_loader_xfer_data -- resizes the xfer buffer and
	 * primes the EP2 OUT DMA). Identical to pongoterm /send. */
	uint32_t newsz = (uint32_t)sz;
	if (ctrl(0x21, 1, 0, 0, &newsz, 4))
		return 1;

	if (bulk_write(buf, sz))
		return 1;
	/* reallocate_loader_xfer_data rounds newsz up to a 512 multiple and
	 * primes the DMA for exactly that many bytes; the Synopsys core only
	 * raises transfer-complete at full size, so pad short tails. */
	uint32_t dma_sz = (uint32_t)((sz + 0x1ff) & ~0x1ff);
	uint32_t pad = dma_sz - (uint32_t)sz;
	if (pad) {
		static uint8_t padbuf[511];
		memset(padbuf, 0, pad);
		if (bulk_write(padbuf, pad))
			return 1;
	}
	printf("uploaded %ld bytes (+%u pad)\n", sz, pad);
	return 0;
}

/* The very first upload attempt used the wLength=0 init, which primed a
 * 1 MiB EP2 OUT DMA (loader_xfer_size = UPLOADSZ) that never completed:
 * usbloader_is_waiting_xfer is stuck at 1 on the device and every new
 * 0x21/1 stalls (kIOUSBPipeStalled). The only thing that clears it is
 * usbloader_xfer_done_cb, i.e. completing the pending bulk OUT. This
 * writes junk into the pending DMA to finish it. */
static int drain(void)
{
	enum { DRAIN_SZ = 1024 * 1024 };
	static uint8_t junk[DRAIN_SZ];
	memset(junk, 0, sizeof(junk));
	printf("draining %d bytes into the pending EP2 DMA\n", DRAIN_SZ);
	return bulk_write(junk, DRAIN_SZ);
}

/* pad <n>: write n zero bytes to EP2 -- completes an in-flight DMA that
 * was primed for a rounded-up size (see upload()). */
static int pad(int n)
{
	if (n < 1 || n > 511) {
		fprintf(stderr, "pad must be 1..511\n");
		return 1;
	}
	static uint8_t padbuf[511];
	memset(padbuf, 0, (size_t)n);
	printf("padding %d bytes into the pending EP2 DMA\n", n);
	return bulk_write(padbuf, n);
}

static int run_cmd(const char *cmd)
{
	size_t len = strlen(cmd) + 1; /* include the NUL? no: \n */
	char *buf = malloc(len + 1);
	snprintf(buf, len + 1, "%s\n", cmd);
	return ctrl(0x21, 3, 0, 0, buf, (uint16_t)strlen(buf));
}

/* read buffered shell stdout: 0xA1/1, wLength 512 (usbloader.c
 * ep0_device_request, IN branch). */
static int query(void)
{
	uint8_t buf[512];
	IOUSBDevRequest request = {
		.bmRequestType = 0xA1,
		.bRequest = 1,
		.wValue = 0,
		.wIndex = 0,
		.wLength = sizeof(buf),
		.pData = buf,
	};
	IOReturn r = (*g_ifc)->ControlRequest(g_ifc, 0, &request);
	if (r != KERN_SUCCESS) {
		fprintf(stderr, "query: 0x%x\n", r);
		return 1;
	}
	if (request.wLenDone)
		fwrite(buf, 1, request.wLenDone, stdout);
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	if (argc < 2) {
		fprintf(stderr,
		        "usage: %s upload <file> | cmd <command> | "
		        "run <file> <command> | drain\n",
		        argv[0]);
		return 2;
	}
	if (open_pongo())
		return 1;

	int rc = 0;
	if (!strcmp(argv[1], "upload") && argc == 3) {
		rc = upload(argv[2]);
	} else if (!strcmp(argv[1], "cmd") && argc == 3) {
		rc = run_cmd(argv[2]);
	} else if (!strcmp(argv[1], "run") && argc == 4) {
		rc = upload(argv[2]) || run_cmd(argv[3]);
	} else if (!strcmp(argv[1], "drain") && argc == 2) {
		rc = drain();
	} else if (!strcmp(argv[1], "pad") && argc == 3) {
		rc = pad(atoi(argv[2]));
	} else if (!strcmp(argv[1], "query") && argc == 2) {
		rc = query();
	} else if (!strcmp(argv[1], "cmdq") && argc == 3) {
		rc = run_cmd(argv[2]) || (usleep(300000), query());
	} else {
		fprintf(stderr, "bad args\n");
		rc = 2;
	}

	close_pongo();
	return rc;
}
