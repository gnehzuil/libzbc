/*
 * This file is part of libzbc.
 *
 * Copyright (C) 2009-2014, HGST, Inc.  All rights reserved.
 *
 * This software is distributed under the terms of the BSD 2-clause license,
 * "as is," without technical support, and WITHOUT ANY WARRANTY, without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. You should have received a copy of the BSD 2-clause license along
 * with libzbc. If not, see  <http://opensource.org/licenses/BSD-2-Clause>.
 * 
 * Authors: Damien Le Moal (damien.lemoal@hgst.com)
 *          Christoph Hellwig (hch@infradead.org)
 */

/***** Including files *****/

#include "zbc.h"

#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <linux/fs.h>

static struct zbc_ops *zbc_ops[] = {
	&zbc_ata_ops,
	&zbc_scsi_ops,
	&zbc_fake_ops,
	NULL
};

/***** Declaration of private funtions *****/

static int
zbc_do_report_zones(zbc_device_t *dev,
                    uint64_t start_lba,
                    enum zbc_reporting_options ro,
                    zbc_zone_t *zones,
                    unsigned int *nr_zones)
{

    /* Nothing much to do here: just call the device command operation */
    return( (dev->zbd_ops->zbd_report_zones)(dev, start_lba, ro, zones, nr_zones) );

}

/***** Definition of private data *****/

int zbc_log_level = ZBC_LOG_ERROR;

/***** Definition of public functions *****/

/**
 * Set library log level.
 */
void
zbc_set_log_level(char *log_level)
{

    if ( log_level ) {
        if ( strcmp(log_level, "none") == 0 ) {
            zbc_log_level = ZBC_LOG_NONE;
        } else if ( strcmp(log_level, "error") == 0 ) {
            zbc_log_level = ZBC_LOG_ERROR;
        } else if ( strcmp(log_level, "info") == 0 ) {
            zbc_log_level = ZBC_LOG_INFO;
        } else if ( strcmp(log_level, "debug") == 0 ) {
            zbc_log_level = ZBC_LOG_DEBUG;
        } else if ( strcmp(log_level, "vdebug") == 0 ) {
            zbc_log_level = ZBC_LOG_VDEBUG;
        } else {
            fprintf(stderr, "Unknown log level \"%s\"\n",
                    log_level);
        }
    }

    return;

}

/**
 * zbc_open - open a (device)file for ZBC access.
 * @filename:   path to the file to be opened
 * @flags:      open mode: O_RDONLY, O_WRONLY or O_RDWR
 * @dev:        opaque ZBC handle
 *
 * Opens the file pointed to by @filename, and returns a handle to it
 * in @dev if it the file is a device special file for a ZBC-capable
 * device.  If the device does not support ZBC this calls returns -EINVAL.
 * Any other error code returned from open(2) can be returned as well.
 */
int
zbc_open(const char *filename,
         int flags,
         zbc_device_t **pdev)
{
    zbc_device_t *dev = NULL;
    int ret = -ENODEV, i;

    /* Test all backends until one accepts the drive */
    for(i = 0; zbc_ops[i] != NULL; i++) {
        ret = zbc_ops[i]->zbd_open(filename, flags, &dev);
	if ( ret == 0 ) {
	    /* This backend accepted the drive */
            dev->zbd_ops = zbc_ops[i];
	    break;
	}
    }

    if ( ret != 0 ) {
	zbc_error("Open device %s failed %d (%s)\n",
		  filename,
		  ret,
		  strerror(-ret));
    } else {
	*pdev = dev;
    }

    return( ret );

}

/**
 * zbc_close - close a ZBC file handle.
 * @dev:                ZBC device handle to close
 *
 * Performs the equivalent to close(2) for a ZBC handle.  Can return any
 * error that close could return.
 */
int
zbc_close(zbc_device_t *dev)
{
    return( dev->zbd_ops->zbd_close(dev) );
}

/**
 * zbc_get_device_info - report misc device information
 * @dev:                (IN) ZBC device handle to report on
 * @info:               (IN) structure that contains ZBC device information
 *
 * Reports information about a ZBD device.  The @info parameter is used to
 * return a device information structure which must be allocated by the caller.
 *
 * Returns -EFAULT if an invalid NULL pointer was specified.
 */
int
zbc_get_device_info(zbc_device_t *dev,
                    zbc_device_info_t *info)
{
    int ret = -EFAULT;

    if ( dev && info ) {
        memcpy(info, &dev->zbd_info, sizeof(zbc_device_info_t));
        ret = 0;
    }

    return( ret );

}

/**
 * zbc_report_zones - Update a list of zone information
 * @dev:                (IN) ZBC device handle to report on
 * @start_lba:          (IN) Start LBA for the first zone to reported
 * @ro:                 (IN) Reporting options
 * @zones:              (IN) Pointer to array of zone information
 * @nr_zones:           (IN/OUT) Number of zones int the array @zones
 *
 * Update an array of zone information previously obtained using zbc_report_zones,
 *
 * Returns -EIO if an error happened when communicating to the device.
 */
int
zbc_report_zones(struct zbc_device *dev,
                 uint64_t start_lba,
                 enum zbc_reporting_options ro,
                 struct zbc_zone *zones,
                 unsigned int *nr_zones)
{
    int ret = 0;

    if ( (! dev) || (! nr_zones) ) {
        return( -EFAULT );
    }

    if ( ! zones ) {

        /* Get number of zones */
        ret = zbc_do_report_zones(dev, start_lba, ro, NULL, nr_zones);

    } else {

        unsigned int n, z = 0, nz = 0;

        /* Get zones info */
        while( nz < *nr_zones ) {

            n = *nr_zones - nz;
            ret = zbc_do_report_zones(dev, start_lba, ro, &zones[z], &n);
            if ( ret != 0 ) {
                zbc_error("Get zones from LBA %llu failed\n",
                          (unsigned long long) start_lba);
                break;
            }

            if ( n == 0 ) {
                break;
            }

            nz += n;
            z += n;
            start_lba = zones[z - 1].zbz_start + zones[z - 1].zbz_length;

        }

        if ( ret == 0 ) {
            *nr_zones = nz;
        }

    }

    return( ret );

}

/**
 * zbc_list_zones - report zones for a ZBC device
 * @dev:                (IN) ZBC device handle to report on
 * @start_lba:          (IN) start LBA for the first zone to reported
 * @ro:                 (IN) Reporting options
 * @zones:              (OUT) pointer for reported zones
 * @nr_zones:           (OUT) number of returned zones
 *
 * Reports the number and details of available zones.  The @zones
 * parameter is used to return an array of zones which is allocated using
 * malloc(3) internally and needs to be freed using free(3).  The number
 * of zones in @zones is returned in @nr_zones.
 *
 * Returns -EIO if an error happened when communicating to the device.
 * Returns -ENOMEM if memory could not be allocated for @zones.
 */
int
zbc_list_zones(struct zbc_device *dev,
               uint64_t start_lba,
               enum zbc_reporting_options ro,
               struct zbc_zone **pzones,
               unsigned int *pnr_zones)
{
    zbc_zone_t *zones = NULL;
    unsigned int nr_zones;
    int ret;

    /* Get total number of zones */
    ret = zbc_report_nr_zones(dev, start_lba, ro, &nr_zones);
    if ( ret < 0 ) {
        return( ret );
    }

    zbc_debug("Device %s: %d zones\n",
              dev->zbd_filename,
              nr_zones);

    /* Allocate zone array */
    zones = (zbc_zone_t *) malloc(sizeof(zbc_zone_t) * nr_zones);
    if ( ! zones ) {
        zbc_error("No memory\n");
        return( -ENOMEM );
    }
    memset(zones, 0, sizeof(zbc_zone_t) * nr_zones);

    /* Get zones info */
    ret = zbc_report_zones(dev, start_lba, ro, zones, &nr_zones);
    if ( ret != 0 ) {
        zbc_error("zbc_report_zones failed\n");
        free(zones);
    } else {
        *pzones = zones;
        *pnr_zones = nr_zones;
    }

    return( ret );

}

/**
 * zbc_reset_write_pointer - reset the write pointer for a ZBC zone
 * @dev:                ZBC device handle to reset on
 * @start_lba:     start LBA for the zone to be reset or -1 to reset all zones
 *
 * Resets the write pointer for a ZBC zone if @start_lba is a valid
 * zone start LBA. If @start_lba specifies -1, the write pointer of all zones
 * is reset. The start LBA for a zone is reported by zbc_report_zones().
 *
 * The zone must be of type ZBC_ZT_SEQUENTIAL_REQ or ZBC_ZT_SEQUENTIAL_PREF
 * and be in the ZBC_ZC_OPEN or ZBC_ZC_FULL state, otherwise -EINVAL
 * will be returned.
 *
 * Returns -EIO if an error happened when communicating to the device.
 */
int
zbc_reset_write_pointer(zbc_device_t *dev,
                        uint64_t start_lba)
{
    int ret;

    /* Reset write pointer */
    ret = (dev->zbd_ops->zbd_reset_wp)(dev, start_lba);
    if ( ret != 0 ) {
        zbc_error("RESET WRITE POINTER command failed\n");
    }

    return( ret );

}

/**
 * zbc_pread - read from a ZBC device
 * @dev:                (IN) ZBC device handle to read from
 * @zone:               (IN) The zone to read in
 * @buf:                (IN) Caller supplied buffer to read into
 * @lba_count:          (IN) Number of LBAs to read
 * @lba_ofst:           (IN) LBA offset where to start reading in @zone
 *
 * This an the equivalent to pread(2) that operates on a ZBC device handle,
 * and uses LBA addressing for the buffer length and I/O offset.
 * It attempts to read in the a number of bytes (@lba_count * logical_block_size)
 * in the zone (@zone) at the offset (@lba_ofst).
 *
 * All errors returned by pread(2) can be returned. On success, the number of
 * logical blocks read is returned.
 */
int32_t
zbc_pread(zbc_device_t *dev,
          zbc_zone_t *zone,
          void *buf,
          uint32_t lba_count,
          uint64_t lba_ofst)
{
    ssize_t ret = -EFAULT;

    if ( dev && zone && buf ) {

	if ( lba_count ) {
	    ret = (dev->zbd_ops->zbd_pread)(dev, zone, buf, lba_count, lba_ofst);
	    if ( ret <= 0 ) {
		zbc_error("Read %u blocks at block %llu + %llu failed %zd (%s)\n",
			  lba_count,
			  (unsigned long long) zbc_zone_start_lba(zone),
			  (unsigned long long) lba_ofst,
			  -ret,
			  strerror(-ret));
	    }
	} else {
	    ret = 0;
	}

    }

    return( ret );

}

/**
 * zbc_pwrite - write to a ZBC device
 * @dev:                (IN) ZBC device handle to write to
 * @zone:               (IN) The zone to write to
 * @buf:                (IN) Caller supplied buffer to write from
 * @lba_count:          (IN) Number of LBAs to write
 * @lba_ofst:           (IN) LBA Offset where to start writing in @zone
 *
 * This an the equivalent to pwrite(2) that operates on a ZBC device handle,
 * and uses LBA addressing for the buffer length. It attempts to writes in the
 * zone (@zone) at the offset (@lba_ofst).
 * The write pointer is updated in case of a succesful call.
 *
 * All errors returned by write(2) can be returned. On success, the number of
 * logical blocks written is returned.
 */
int32_t
zbc_pwrite(zbc_device_t *dev,
           zbc_zone_t *zone,
           const void *buf,
           uint32_t lba_count,
           uint64_t lba_ofst)
{
    ssize_t ret = -EFAULT;

    if ( dev && zone && buf ) {

	if ( lba_count ) {

	    /* Execute write */
	    ret = (dev->zbd_ops->zbd_pwrite)(dev, zone, buf, lba_count, lba_ofst);
	    if ( ret <= 0 ) {
		zbc_error("Write %u blocks at block %llu + %llu failed %zd (%s)\n",
			  lba_count,
			  (unsigned long long) zbc_zone_start_lba(zone),
			  (unsigned long long) lba_ofst,
			  -ret,
			  strerror(-ret));
	    }

	} else {

	    ret = 0;

	}

    }

    return( ret );

}

/**
 * zbc_flush - flush to a ZBC device cache
 * @dev:                (IN) ZBC device handle to flush
 *
 * This an the equivalent to fsync/fdatasunc but operates at the device cache level.
 */
int
zbc_flush(zbc_device_t *dev)
{

    return( (dev->zbd_ops->zbd_flush)(dev, 0, 0, 0) );

}

/**
 * zbc_set_zones - Configure zones of a "hacked" ZBC device
 * @dev:                (IN) ZBC device handle of the device to configure
 * @conv_sz:            (IN) Size in logical sectors of the conventional zone (zone 0). This can be 0.
 * @seq_sz:             (IN) Size in logical sectors of sequential write required zones. This cannot be 0.
 *
 * This executes the non-standard SET ZONES command to change the zone configuration of a ZBC drive.
 */
int
zbc_set_zones(zbc_device_t *dev,
              uint64_t conv_sz,
              uint64_t seq_sz)
{
    int ret;

    /* Do this only if supported */
    if ( dev->zbd_ops->zbd_set_zones ) {
        ret = (dev->zbd_ops->zbd_set_zones)(dev, conv_sz, seq_sz);
    } else {
        ret = -ENXIO;
    }

    return( ret );

}

/**
 * zbc_set_write_pointer - Change the value of a zone write pointer
 * @dev:                (IN) ZBC device handle of the device to configure
 * @zone:               (IN) The zone to configure
 * @write_pointer:      (IN) New value of the write pointer (must be at least equal to the zone start LBA
 *                           (zone empty) and at most equal to the zone last LBA plus 1 (zone full).
 *
 * This executes the non-standard SET ZONES command to change the zone configuration of a ZBC drive.
 */
int
zbc_set_write_pointer(struct zbc_device *dev,
                      uint64_t start_lba,
                      uint64_t write_pointer)
{
    int ret;

    /* Do this only if supported */
    if ( dev->zbd_ops->zbd_set_wp ) {
        ret = (dev->zbd_ops->zbd_set_wp)(dev, start_lba, write_pointer);
    } else {
        ret = -ENXIO;
    }

    return( ret );

}


