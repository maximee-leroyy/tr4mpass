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
	return 0;
}

static void close_pongo(void)
{
	(*g_dev)->USBDeviceClose(g_dev);
	(*g_dev)->Release(g_dev);
}

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
	IOReturn r = (*g_dev)->DeviceRequest(g_dev, &request);
	if (r != KERN_SUCCESS) {
		fprintf(stderr, "ctrl 0x%02x/%d: 0x%x\n", bm, req, r);
		return 1;
	}
	return 0;
}

static int find_ep2_out(uint8_t *out_ep)
{
	IOUSBFindInterfaceRequest ifreq = {
		.bInterfaceClass = kIOUSBFindInterfaceDontCare,
		.bInterfaceSubClass = kIOUSBFindInterfaceDontCare,
		.bInterfaceProtocol = kIOUSBFindInterfaceDontCare,
		.bAlternateSetting = kIOUSBFindInterfaceDontCare,
	};
	io_iterator_t iter = IO_OBJECT_NULL;
	IOReturn r = (*g_dev)->CreateInterfaceIterator(g_dev, &ifreq,
	                                               &iter);
	if (r != KERN_SUCCESS)
		return 1;
	io_service_t ifsrv;
	int ret = 1;
	while ((ifsrv = IOIteratorNext(iter))) {
		IOCFPlugInInterface **plug = NULL;
		SInt32 score = 0;
		if (IOCreatePlugInInterfaceForService(
		        ifsrv, kIOUSBInterfaceUserClientTypeID,
		        kIOCFPlugInInterfaceID, &plug, &score)
		    == KERN_SUCCESS) {
			IOUSBInterfaceInterface **ifc = NULL;
			if ((*plug)->QueryInterface(
			        plug,
			        CFUUIDGetUUIDBytes(
			            kIOUSBInterfaceInterfaceID),
			        (LPVOID *)&ifc) == 0) {
				(*ifc)->USBInterfaceOpen(ifc);
				UInt8 neps = 0;
				(*ifc)->GetNumEndpoints(ifc, &neps);
				for (UInt8 i = 0; i < neps; i++) {
					UInt8 eptype = 0;
					UInt16 epnum = 0;
					if ((*ifc)->GetEndpointProperties(
					        ifc, 0, i, 0, &eptype,
					        &epnum, NULL)
					        == KERN_SUCCESS &&
					    epnum == 2 && eptype == 2) {
						/* EP2 bulk OUT */
						*out_ep = 0x02;
						ret = 0;
						break;
					}
				}
				(*ifc)->USBInterfaceClose(ifc);
				(*ifc)->Release(ifc);
			}
			(*plug)->Release(plug);
		}
		IOObjectRelease(ifsrv);
		if (!ret)
			break;
	}
	IOObjectRelease(iter);
	return ret;
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

	/* init bulk upload: 0x21/1, wLength 0 */
	if (ctrl(0x21, 1, 0, 0, NULL, 0))
		return 1;

	/* find the EP2 bulk OUT endpoint and pump the data */
	uint8_t ep = 0;
	if (find_ep2_out(&ep)) {
		fprintf(stderr, "no bulk OUT endpoint found\n");
		return 1;
	}
	printf("bulk OUT endpoint: 0x%02x\n", ep);

	/* open the interface again for the write */
	IOUSBFindInterfaceRequest ifreq = {
		.bInterfaceClass = kIOUSBFindInterfaceDontCare,
		.bInterfaceSubClass = kIOUSBFindInterfaceDontCare,
		.bInterfaceProtocol = kIOUSBFindInterfaceDontCare,
		.bAlternateSetting = kIOUSBFindInterfaceDontCare,
	};
	io_iterator_t iter = IO_OBJECT_NULL;
	if ((*g_dev)->CreateInterfaceIterator(g_dev, &ifreq, &iter)
	    != KERN_SUCCESS) {
		fprintf(stderr, "interface iterator failed\n");
		return 1;
	}
	io_service_t ifsrv = IOIteratorNext(iter);
	IOObjectRelease(iter);
	if (!ifsrv) {
		fprintf(stderr, "no interface\n");
		return 1;
	}
	IOCFPlugInInterface **plug = NULL;
	SInt32 score = 0;
	IOReturn r = IOCreatePlugInInterfaceForService(
	    ifsrv, kIOUSBInterfaceUserClientTypeID,
	    kIOCFPlugInInterfaceID, &plug, &score);
	IOObjectRelease(ifsrv);
	if (r != KERN_SUCCESS) {
		fprintf(stderr, "interface plug-in failed\n");
		return 1;
	}
	IOUSBInterfaceInterface **ifc = NULL;
	(*plug)->QueryInterface(
	    plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID),
	    (LPVOID *)&ifc);
	(*plug)->Release(plug);

	r = (*ifc)->USBInterfaceOpen(ifc);
	if (r != KERN_SUCCESS) {
		fprintf(stderr, "USBInterfaceOpen: 0x%x\n", r);
		(*ifc)->Release(ifc);
		return 1;
	}

	/* 16 KiB chunks; WritePipe blocks until the device accepts */
	long off = 0;
	while (off < sz) {
		long chunk = (sz - off > 16384) ? 16384 : (sz - off);
		r = (*ifc)->WritePipe(ifc, ep, buf + off, (UInt32)chunk);
		if (r != KERN_SUCCESS) {
			fprintf(stderr, "WritePipe at %ld: 0x%x\n", off, r);
			(*ifc)->USBInterfaceClose(ifc);
			(*ifc)->Release(ifc);
			return 1;
		}
		off += chunk;
	}
	(*ifc)->USBInterfaceClose(ifc);
	(*ifc)->Release(ifc);
	printf("uploaded %ld bytes\n", off);
	return 0;
}

static int run_cmd(const char *cmd)
{
	size_t len = strlen(cmd) + 1; /* include the NUL? no: \n */
	char *buf = malloc(len + 1);
	snprintf(buf, len + 1, "%s\n", cmd);
	return ctrl(0x21, 3, 0, 0, buf, (uint16_t)strlen(buf));
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
		        "usage: %s upload <file> | cmd <command> | "
		        "run <file> <command>\n",
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
	} else {
		fprintf(stderr, "bad args\n");
		rc = 2;
	}

	close_pongo();
	return rc;
}
