/*
 * This file contains helper functions for Kernel Mode Setting using
 * dumb buffers. It is based on the DRM Modesetting example by
 * David Herrmann <dh.herrmann@googlemail.com> present at
 * https://github.com/dvdhrm/docs/blob/master/drm-howto/modeset.c
 *
 * Parts of the code have been taken directly from the example.
 * Other parts have been modified to support the requried functionality.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "modeset.h"

/* modeset_open(out, node): This small helper function opens the DRM device
 * which is given as @node. The new fd is stored in @out on success. On failure,
 * a negative error code is returned.
 * After opening the file, we also check for the DRM_CAP_DUMB_BUFFER capability.
 * If the driver supports this capability, we can create simple memory-mapped
 * buffers without any driver-dependent code. As we want to avoid any radeon,
 * nvidia, intel, etc. specific code, we depend on DUMB_BUFFERs here.
 */
int modeset_open(int *out, const char *node)
{
	int fd, ret;
	uint64_t has_dumb;

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ret = -errno;
		fprintf(stderr, "cannot open '%s': %m\n", node);
		return ret;
	}

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 ||
	    !has_dumb) {
		fprintf(stderr, "drm device '%s' does not support dumb buffers\n",
			node);
		close(fd);
		return -EOPNOTSUPP;
	}

	*out = fd;
	return 0;
}

extern struct modeset_dev *modeset_list = NULL;

/*
 * So as next step we need to actually prepare all connectors that we find. We
 * do this in this little helper function:
 *
 * modeset_prepare(fd): This helper function takes the DRM fd as argument and
 * then simply retrieves the resource-info from the device. It then iterates
 * through all connectors and calls other helper functions to initialize this
 * connector (described later on).
 * If the initialization was successful, we simply add this object as new device
 * into the global modeset device list.
 *
 * The resource-structure contains a list of all connector-IDs. We use the
 * helper function drmModeGetConnector() to retrieve more information on each
 * connector. After we are done with it, we free it again with
 * drmModeFreeConnector().
 * Our helper modeset_setup_dev() returns -ENOENT if the connector is currently
 * unused and no monitor is plugged in. So we can ignore this connector.
 */

int modeset_prepare(int fd)
{
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct modeset_dev *dev;
	int ret;

	/* retrieve resources */
	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n",
			errno);
		return -errno;
	}

	/* iterate all connectors */
	for (i = 0; i < res->count_connectors; ++i) {
		/* get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
				i, res->connectors[i], errno);
			continue;
		}

		/* create a device structure */
		dev = malloc(sizeof(*dev));
		memset(dev, 0, sizeof(*dev));
		dev->conn = conn->connector_id;

		/* call helper function to prepare this connector */
		ret = modeset_setup_dev(fd, res, conn, dev);
		if (ret) {
			if (ret != -ENOENT) {
				errno = -ret;
				fprintf(stderr, "cannot setup device for connector %u:%u (%d): %m\n",
					i, res->connectors[i], errno);
			}
			free(dev);
			drmModeFreeConnector(conn);
			continue;
		}

		/* free connector data and link device into global list */
		drmModeFreeConnector(conn);
		dev->next = modeset_list;
		modeset_list = dev;
	}

	/* free resources again */
	drmModeFreeResources(res);
	return 0;
}

/*
 * Now we dig deeper into setting up a single connector. As described earlier,
 * we need to check several things first:
 *   * If the connector is currently unused, that is, no monitor is plugged in,
 *     then we can ignore it.
 *   * We have to find a suitable resolution and refresh-rate. All this is
 *     available in drmModeModeInfo structures saved for each crtc. We simply
 *     use the first mode that is available. This is always the mode with the
 *     highest resolution.
 *     A more sophisticated mode-selection should be done in real applications,
 *     though.
 *   * Then we need to find an CRTC that can drive this connector. A CRTC is an
 *     internal resource of each graphics-card. The number of CRTCs controls how
 *     many connectors can be controlled indepedently. That is, a graphics-cards
 *     may have more connectors than CRTCs, which means, not all monitors can be
 *     controlled independently.
 *     There is actually the possibility to control multiple connectors via a
 *     single CRTC if the monitors should display the same content. However, we
 *     do not make use of this here.
 *     So think of connectors as pipelines to the connected monitors and the
 *     CRTCs are the controllers that manage which data goes to which pipeline.
 *     If there are more pipelines than CRTCs, then we cannot control all of
 *     them at the same time.
 *   * We need to create a framebuffer for this connector. A framebuffer is a
 *     memory buffer that we can write XRGB32 data into. So we use this to
 *     render our graphics and then the CRTC can scan-out this data from the
 *     framebuffer onto the monitor.
 */

int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev)
{
	int ret;

	/* check if a monitor is connected */
	if (conn->connection != DRM_MODE_CONNECTED) {
		fprintf(stderr, "ignoring unused connector %u\n",
			conn->connector_id);
		return -ENOENT;
	}

	/* check if there is at least one valid mode */
	if (conn->count_modes == 0) {
		fprintf(stderr, "no valid mode for connector %u\n",
			conn->connector_id);
		return -EFAULT;
	}

	/* copy the mode information into our device structure */
	memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));
	dev->width = conn->modes[0].hdisplay;
	dev->height = conn->modes[0].vdisplay;
	fprintf(stderr, "mode for connector %u is %ux%u\n",
		conn->connector_id, dev->width, dev->height);

	/* find a crtc for this connector */
	ret = modeset_find_crtc(fd, res, conn, dev);
	if (ret) {
		fprintf(stderr, "no valid crtc for connector %u\n",
			conn->connector_id);
		return ret;
	}

	/* create a framebuffer for this CRTC */
	ret = modeset_create_fb(fd, dev);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer for connector %u\n",
			conn->connector_id);
		return ret;
	}

	return 0;
}

/*
 * modeset_find_crtc(fd, res, conn, dev): This small helper tries to find a
 * suitable CRTC for the given connector. We have actually have to introduce one
 * more DRM object to make this more clear: Encoders.
 * Encoders help the CRTC to convert data from a framebuffer into the right
 * format that can be used for the chosen connector. We do not have to
 * understand any more of these conversions to make use of it. However, you must
 * know that each connector has a limited list of encoders that it can use. And
 * each encoder can only work with a limited list of CRTCs. So what we do is
 * trying each encoder that is available and looking for a CRTC that this
 * encoder can work with. If we find the first working combination, we are happy
 * and write it into the @dev structure.
 * But before iterating all available encoders, we first try the currently
 * active encoder+crtc on a connector to avoid a full modeset.
 *
 * However, before we can use a CRTC we must make sure that no other device,
 * that we setup previously, is already using this CRTC. Remember, we can only
 * drive one connector per CRTC! So we simply iterate through the "modeset_list"
 * of previously setup devices and check that this CRTC wasn't used before.
 * Otherwise, we continue with the next CRTC/Encoder combination.
 */

int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev)
{
	drmModeEncoder *enc;
	unsigned int i, j;
	int32_t crtc;
	struct modeset_dev *iter;

	/* first try the currently conected encoder+crtc */
	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;

	if (enc) {
		if (enc->crtc_id) {
			crtc = enc->crtc_id;
			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc == crtc) {
					crtc = -1;
					break;
				}
			}

			if (crtc >= 0) {
				drmModeFreeEncoder(enc);
				dev->crtc = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	/* If the connector is not currently bound to an encoder or if the
	 * encoder+crtc is already used by another connector (actually unlikely
	 * but lets be safe), iterate all other available encoders to find a
	 * matching CRTC. */
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n",
				i, conn->encoders[i], errno);
			continue;
		}

		/* iterate all global CRTCs */
		for (j = 0; j < res->count_crtcs; ++j) {
			/* check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			/* check that no other device already uses this CRTC */
			crtc = res->crtcs[j];
			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc == crtc) {
					crtc = -1;
					break;
				}
			}

			/* we have found a CRTC, so save it and return */
			if (crtc >= 0) {
				drmModeFreeEncoder(enc);
				dev->crtc = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	fprintf(stderr, "cannot find suitable CRTC for connector %u\n",
		conn->connector_id);
	return -ENOENT;
}

/*
 * modeset_create_fb(fd, dev): After we have found a crtc+connector+mode
 * combination, we need to actually create a suitable framebuffer that we can
 * use with it. There are actually two ways to do that:
 *   * We can create a so called "dumb buffer". This is a buffer that we can
 *     memory-map via mmap() and every driver supports this. We can use it for
 *     unaccelerated software rendering on the CPU.
 *   * We can use libgbm to create buffers available for hardware-acceleration.
 *     libgbm is an abstraction layer that creates these buffers for each
 *     available DRM driver. As there is no generic API for this, each driver
 *     provides its own way to create these buffers.
 *     We can then use such buffers to create OpenGL contexts with the mesa3D
 *     library.
 * We use the first solution here as it is much simpler and doesn't require any
 * external libraries. However, if you want to use hardware-acceleration via
 * OpenGL, it is actually pretty easy to create such buffers with libgbm and
 * libEGL. But this is beyond the scope of this document.
 *
 * So what we do is requesting a new dumb-buffer from the driver. We specify the
 * same size as the current mode that we selected for the connector.
 * Then we request the driver to prepare this buffer for memory mapping. After
 * that we perform the actual mmap() call. So we can now access the framebuffer
 * memory directly via the dev->map memory map.
 */

int modeset_create_fb(int fd, struct modeset_dev *dev)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;
	int ret;

	/* create dumb buffer */
	memset(&creq, 0, sizeof(creq));
	creq.width = dev->width;
	creq.height = dev->height;
	creq.bpp = 32;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		fprintf(stderr, "cannot create dumb buffer (%d): %m\n",
			errno);
		return -errno;
	}
	dev->stride = creq.pitch;
	dev->size = creq.size;
	dev->handle = creq.handle;
	/* create framebuffer object for the dumb-buffer */
	ret = drmModeAddFB(fd, dev->width, dev->height, 24, 32, dev->stride,
			   dev->handle, &dev->fb);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_destroy;
	}

	/* prepare buffer for memory mapping */
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = dev->handle;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		fprintf(stderr, "cannot map dumb buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}

	/* perform actual memory mapping */
	dev->map = mmap(0, dev->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		        fd, mreq.offset);
	if (dev->map == MAP_FAILED) {
		fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}

	/* clear the framebuffer to 0 */
	memset(dev->map, 0, dev->size);

	return 0;

err_fb:
	drmModeRmFB(fd, dev->fb);
err_destroy:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = dev->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}



 /* We open the device via modeset_open(). modeset_prepare()
  * prepares all connectors and we can loop over "modeset_list" and call
  * drmModeSetCrtc() on every CRTC/connector combination.
  *
  * Before calling drmModeSetCrtc() we also save the current CRTC configuration.
  * This is used in modeset_cleanup() to restore the CRTC to the same mode as was
  * before we changed it.
  * If we don't do this, the screen will stay blank after we exit until another
  * application performs modesetting itself.
  */
int init_modeset(char *device) {
	int ret, fd;
	const char *card;
	struct modeset_dev *iter;
	card = device;

	fprintf(stderr, "using card '%s'\n", card);

	/* open the DRM device */
	ret = modeset_open(&fd, card);
	drmSetMaster(fd);

	if (ret)
		goto out_return;

	/* prepare all connectors and CRTCs */
	ret = modeset_prepare(fd);
	if (ret)
		goto out_close;

	/* perform actual modesetting on each found connector+CRTC */
	for (iter = modeset_list; iter; iter = iter->next) {
		iter->saved_crtc = drmModeGetCrtc(fd, iter->crtc);
		ret = drmModeSetCrtc(fd, iter->crtc, iter->fb, 0, 0,
						 &iter->conn, 1, &iter->mode);
		if (ret)
			fprintf(stderr, "cannot set CRTC for connector %u (%d): %m\n",
				iter->conn, errno);
	}
	return fd;
out_close:
	close(fd);
out_return:
	if (ret) {
		errno = -ret;
		fprintf(stderr, "modeset failed with error %d: %m\n", errno);
	} else {
		fprintf(stderr, "exiting\n");
	}
	drmDropMaster(fd);
	return ret;

}

/*
 * modeset_draw(): This draws a solid color into all configured framebuffers.
 * It takes two buffers and has 4 display modes - LR, RL, TB, BT
 * LR - Original frame on left half of the screen and modified frame on right half.
 * RL - Original frame on right half of the screen and modified frame on left half.
 * TB - Original frame on top half of the screen and modified frame on bottom half
 * BT - Original frame on bottom half of the screen and modified frame on top half
 */

void modeset_draw(uint32_t *original_buffer, uint32_t * buff, int display_mode)
{
	unsigned int j, k, off;
	struct modeset_dev *iter;
	uint32_t *temp;
	iter = modeset_list;
	if(display_mode == 0 || display_mode == 1) {
		if(display_mode == 1) {
			 temp = original_buffer;
			 original_buffer = buff;
			 buff = temp;
		}
		// draw left
		for(j=0; j< iter->height; ++j ){
			for (k = 0; k < iter->width/2; k++) {
				off = iter->stride * j + k * 4;
				*(uint32_t*)&iter->map[off] = original_buffer[j*iter->width+k*2];
			}
		}
		//draw right
		for(j=0; j< iter->height; ++j ){
			for (k = iter->width/2; k < iter->width; ++k) {
				off = iter->stride * j + k * 4;
				*(uint32_t*)&iter->map[off] = buff[j*iter->width+((k-iter->width/2)*2)];
			}
		}
	}
	if(display_mode == 2 || display_mode == 3) {
		if(display_mode == 3) {
			 temp = original_buffer;
			 original_buffer = buff;
			 buff = temp;
		}
		// draw up
		for(j=0;j<iter->height/2;j++) {
			for (k = 0; k < iter->width; ++k) {
				off = iter->stride * j + k * 4;
				*(uint32_t*)&iter->map[off] = original_buffer[j*iter->width+k];
			}
		}
		// draw down
		for(j=iter->height/2;j<iter->height;j++) {
			for (k = 0; k < iter->width; ++k) {
				off = iter->stride * j + k * 4;
				*(uint32_t*)&iter->map[off] = buff[j*iter->width+k];
			}
		}
	}

}

/*
 * modeset_cleanup(fd): This cleans up all the devices we created during
 * modeset_prepare(). It resets the CRTCs to their saved states and deallocates
 * all memory.
 * It should be pretty obvious how all of this works.
 */

void modeset_cleanup(int fd)
{
	struct modeset_dev *iter;
	struct drm_mode_destroy_dumb dreq;

	while (modeset_list) {
		/* remove from global list */
		iter = modeset_list;
		modeset_list = iter->next;

		/* restore saved CRTC configuration */
		drmModeSetCrtc(fd,
			       iter->saved_crtc->crtc_id,
			       iter->saved_crtc->buffer_id,
			       iter->saved_crtc->x,
			       iter->saved_crtc->y,
			       &iter->conn,
			       1,
			       &iter->saved_crtc->mode);
		drmModeFreeCrtc(iter->saved_crtc);

		/* unmap buffer */
		munmap(iter->map, iter->size);

		/* delete framebuffer */
		drmModeRmFB(fd, iter->fb);

		/* delete dumb buffer */
		memset(&dreq, 0, sizeof(dreq));
		dreq.handle = iter->handle;
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

		/* free allocated memory */
		free(iter);

    /* close the device */
		close(fd);
	}
}
